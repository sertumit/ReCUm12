# -*- coding: utf-8 -*-
# Windows RS-485 Controller for pump_proto_sim.py
# GTK3 + PySerial, Glade KULLANMADAN (Builder-free) â€” kararlÄ± Ã§alÄ±ÅŸsÄ±n diye.

import os, sys, threading, queue, time
import datetime, pathlib, io, time
import serial
import serial.tools.list_ports
import gi,math
gi.require_version("Gtk","3.0")
from gi.repository import Gtk, GLib, Pango, Gdk
#
# CSV logger (opsiyonel import — yoksa None’a düş)
try:
    from logs import CsvLogger
except Exception:
    CsvLogger = None
import pathlib  # zaten üstte import var; pathlib.Path kullanacağız
# --- AŞAMA-1: Glade/CSS yol sabitleri ve yardımcılar (UI düzenine dokunmadan) ---
BASE_DIR = pathlib.Path(__file__).parent
GUI_DIR  = BASE_DIR / "gui" / "resources"
RES_DIR  = GUI_DIR

def _read_text(p: pathlib.Path) -> str:
    try:
        return p.read_text(encoding="utf-8")
    except Exception:
        return ""

def _css_with_absolute_urls(css_text: str, res_dir: pathlib.Path) -> str:
    """
    CSS içindeki url("...") yollarını mutlak file:// URL'lerine çevirir (runtime).
    Diskteki dosyayı değiştirmez.
    """
    import re
    def repl(m):
        raw = m.group(1).strip().strip('\'"')
        if raw.startswith("file://") or "://" in raw:
            return f'url("{raw}")'
        abs_p = (res_dir / raw).resolve()
        return f'url("file://{abs_p.as_posix()}")'
    return re.sub(r'url\(([^)]+)\)', repl, css_text)

APP_VERSION = "v20.C02-01"
APP_TITLE = f"Win Pump RS-485 Controller [{APP_VERSION}]"
DEFAULT_PORT = "COM5"       # kendi COMâ€™unu yaz
DEFAULT_BAUD = 9600
DEFAULT_ADDR = 0x50

STX = 0x02
ETX = 0x03
TRAIL = 0xFA

def crc16_ibm(bs: bytes, init: int = 0x0000) -> int:
     """CRC16-IBM/Modbus (poly=0xA001), MSB,LSB.
     Not: Mepsan sahada init=0x0000 kullanıyor (YAT logundan teyit)."""
     crc = init
     for b in bs:
         crc ^= b
         for _ in range(8):
             if crc & 1:
                 crc = (crc >> 1) ^ 0xA001
             else:
                 crc >>= 1
         crc &= 0xFFFF
     return crc

def hexline(data: bytes) -> str:
    return data.hex().upper()

def _bcd4_to_int(bs: bytes) -> int:
    """
    4 byte (8 nibble) BCD'yi tamsayıya çevirir.
   Örn: b'\x00\x00\x01\x23' -> 123
    Geçersiz nibble'ları (>=0xA) 0 sayar.
    """
    if len(bs) != 4:
        return 0
    val = 0
    for b in bs:
        hi, lo = (b >> 4) & 0xF, b & 0xF
        val = val * 10 + (hi if hi < 10 else 0)
        val = val * 10 + (lo if lo < 10 else 0)
    return val

def _bcd5_to_int(bs: bytes) -> int:
    """
    5 byte (10 haneli) BCD'yi tamsayıya çevirir.
    Örn: b'\\x00\\x00\\x00\\x01\\x23' -> 123
    Geçersiz nibble'lar (>=0xA) 0 sayılır.
    """
    if len(bs) < 5:
        return 0
    val = 0
    for b in bs[:5]:
        hi, lo = (b >> 4) & 0xF, b & 0xF
        val = val * 10 + (hi if hi < 10 else 0)
        val = val * 10 + (lo if lo < 10 else 0)
    return val

def _int_to_bcd4(val: int) -> bytes:
    """
    Tamsayıyı 4 byte (8 haneli) BCD'e çevirir.
    Örn: 800 -> b'\\x00\\x00\\x08\\x00' (8,00 L preset).
    Geçerli aralık: 0..99_999_999 (dışına taşarsa kırpılır).
    """
    if val < 0:
        val = 0
    if val > 99_999_999:
        val = 99_999_999
    s = f"{val:08d}"  # 8 hane
    out = bytearray(4)
    for i in range(4):
        hi = int(s[2 * i])
        lo = int(s[2 * i + 1])
        out[i] = (hi << 4) | lo
    return bytes(out)

class SerialReader(threading.Thread):
    def __init__(self, ser: serial.Serial, rxq: queue.Queue, on_err, raw_cb=None):
        super().__init__(daemon=True)
        self.ser = ser
        self.rxq = rxq
        self.on_err = on_err
        self._stop = threading.Event()
        self.buf = bytearray()
        # raw_cb: her okunan chunk'ı (bytes) ham olarak loglamak için opsiyonel callback
        self._raw_cb = raw_cb
    def run(self):
        try:
            while not self._stop.is_set():
                try:
                    chunk = self.ser.read(4096)
                except serial.SerialException as e:
                    self.on_err(f"SerialException: {e}")
                    break
                if not chunk:
                    continue
                # Ham chunk'ı isteğe bağlı debug callback'ine ilet
                if self._raw_cb is not None:
                    try:
                        self._raw_cb(bytes(chunk))
                    except Exception:
                        # Debug loglaması asla reader'ı öldürmesin
                        pass
                self.buf.extend(chunk)
                # Çerçeve ayıklama:
                #  (A) Normal: ... ETX(0x03) + TRAIL(0xFA)
                #  (B) Kısa (min): 3 bayt ve FA ile biter: 0x50 0x20/0xC0 0xFA
                while True:
                    trl = self.buf.find(b"\xFA")
                    if trl == -1:
                        break  # henüz FA yok, daha fazla veri bekle
                    cand = bytes(self.buf[:trl+1])
                    # Önce "normal" ETX+FA şablonu var mı diye bakalım
                    etx = cand.rfind(b"\x03")
                    if etx != -1 and etx < trl and len(cand) >= 7:
                        frame = cand
                        del self.buf[:trl+1]
                        self.rxq.put(frame)
                        continue
                    # Kısa (min) çerçeve mi? 3 bayt, 0x50 ? 0xFA
                    if len(cand) == 3 and cand[0] == 0x50 and cand[-1] == 0xFA:
                        frame = cand
                        del self.buf[:trl+1]
                        self.rxq.put(frame)
                        continue
                    # Bu FA henüz tam bir çerçeve oluşturmuyor; daha fazla veri bekle
                    # (ör. gürültü/eksik ETX)
                    break
        except Exception as e:
            self.on_err(str(e))

    def stop(self):
        self._stop.set()

class MainWin(Gtk.Window):
    def __init__(self):
        super().__init__(title=APP_TITLE)
        self._shutting_down = False  # güvenli kapatma bayrağı
        # Serbest yeniden boyutlandırma (üst sınır yok; makul alt sınır var)
        self.set_default_size(800, 640)
        try:
            self.set_size_request(-1, -1)  # sabitleme yok
        except Exception:
            pass
        # --- AŞAMA-1: Glade & CSS yükle (TX/RX Glade’e taşınmaz; mevcut layout korunur)
        try:
            self._init_glade_and_css()
        except Exception as e:
            sys.stderr.write(f"[GLADE-INIT-WARN] {e}\n")

        try:
            self.set_resizable(True)
            geom = Gdk.Geometry()
            geom.min_width = 520
            geom.min_height = 560           # gerekiyorsa 480’e çekilebilir
            # Sadece MIN_SIZE ipucu ver (MAX_SIZE gereksiz ve kısıtlayıcı olabilir)
            self.set_geometry_hints(None, geom, Gdk.WindowHints.MIN_SIZE)
        except Exception:
            pass

        # --- LOG dosyası (disk) ---
        self._logf = self._open_log()
        self._log("=== CONTROLLER START ===")

        # Hızlı debug satırı: yalnızca disk loguna düş (GUI'de [DBG] görünmez)
        def _dbg(msg: str):
            try:
                # Üretim modunda sadece dosyaya yaz; kullanıcıya görünür
                # [DBG] satırı üretme (PARSED/TX/RX'e düşmez).
                self._log(f"[DBG] {msg}")
            except Exception:
                # Debug log hatası asla uygulamayı bozmasın
                pass
        self._dbg = _dbg
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        for s in (outer,):
            s.set_margin_start(8); s.set_margin_end(8); s.set_margin_top(8); s.set_margin_bottom(8)
        # Glade kökü zaten eklendiyse, elde kurulan UI'yi pencereye eklemeyelim
        if self.get_child() is None:
            self.add(outer)

         # --- Üst kısım: Port/Baud/Parity/Stop/Timeout/Addr ve Connect
        top = Gtk.Grid(column_spacing=8, row_spacing=8)
        outer.pack_start(top, False, False, 0)

        self.cmb_port = Gtk.ComboBoxText()
        self._refresh_ports()
        self.cmb_port.set_active(0)
        top.attach(Gtk.Label(label="Port:"), 0, 0, 1, 1)
        top.attach(self.cmb_port, 1, 0, 1, 1)

        self.spn_baud = Gtk.SpinButton.new_with_range(300, 1000000, 100)
        self.spn_baud.set_value(DEFAULT_BAUD)
        top.attach(Gtk.Label(label="Baud:"), 2, 0, 1, 1)
        top.attach(self.spn_baud, 3, 0, 1, 1)

        # Parite (None/Even/Odd)
        self.cmb_par = Gtk.ComboBoxText()
        for s in ("None", "Even", "Odd"):
            self.cmb_par.append_text(s)
        self.cmb_par.set_active(2)  # default Odd
        top.attach(Gtk.Label(label="Parity:"), 4, 0, 1, 1)
        top.attach(self.cmb_par, 5, 0, 1, 1)

        # Stop bits (1 / 2)
        self.cmb_stop = Gtk.ComboBoxText()
        for s in ("1", "2"):
            self.cmb_stop.append_text(s)
        self.cmb_stop.set_active(0)  # default 1
        top.attach(Gtk.Label(label="Stop:"), 6, 0, 1, 1)
        top.attach(self.cmb_stop, 7, 0, 1, 1)

        # Timeout (ms)
        self.spn_tout = Gtk.SpinButton.new_with_range(5, 2000, 5)
        self.spn_tout.set_value(50)  # default 50ms
        top.attach(Gtk.Label(label="Timeout (ms):"), 8, 0, 1, 1)
        top.attach(self.spn_tout, 9, 0, 1, 1)
        self.spn_addr = Gtk.SpinButton.new_with_range(0, 255, 1)
        self.spn_addr.set_value(DEFAULT_ADDR)
        top.attach(Gtk.Label(label="Addr (hex):"), 10, 0, 1, 1)
        top.attach(self.spn_addr, 11, 0, 1, 1)

        self.btn_conn = Gtk.Button(label="Open")
        self.btn_conn.connect("clicked", self.on_open_clicked)
        top.attach(self.btn_conn, 12, 0, 1, 1)
        # Auto-open kullanacağımız için 'Open' butonunu gizle/devre dışı bırak
        try:
            self.btn_conn.set_sensitive(False)
            self.btn_conn.set_no_show_all(True)
            self.btn_conn.hide()
        except Exception:
            pass
        # --- DC durum etiketleri
        stat = Gtk.Grid(column_spacing=8, row_spacing=8)
        outer.pack_start(stat, False, False, 0)

        self.lbl_dc1 = Gtk.Label(label="DC1: -"); self._bold(self.lbl_dc1)
        self.lbl_dc2 = Gtk.Label(label="DC2: -")
        self.lbl_dc3 = Gtk.Label(label="DC3: -")
        # NEW: DC101 totalizer özeti
        self.lbl_dc101 = Gtk.Label(label="DC101: -")
        stat.attach(self.lbl_dc1, 0, 0, 1, 1)
        stat.attach(self.lbl_dc2, 1, 0, 1, 1)
        stat.attach(self.lbl_dc3, 2, 0, 1, 1)
        stat.attach(self.lbl_dc101, 3, 0, 1, 1)

        # --- Durum LED satÄ±rÄ± (yeni) ---
        state_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        state_row.set_border_width(4)
        outer.pack_start(state_row, False, False, 0)

        self._led_color = (0.75, 0.75, 0.75)   # RESET=gray
        self.led = Gtk.DrawingArea()
        self.led.set_size_request(14, 14)
        self.led.connect("draw", self._on_led_draw)
        state_row.pack_start(self.led, False, False, 0)

        self.lbl_state = Gtk.Label(label="RESET")
        self.lbl_state.set_xalign(0.0)
        state_row.pack_start(self.lbl_state, False, False, 0)

        # Yeni: Nozzle durumu etiketi
        self._nozzle_out = False
        self.lbl_nozzle = Gtk.Label(label="NOZZLE: IN")
        self.lbl_nozzle.set_xalign(0.0)
        # biraz boÅŸluk iÃ§in padding benzeri
        spacer = Gtk.Box()
        spacer.set_size_request(12, 1)
        state_row.pack_start(spacer, False, False, 0)
        state_row.pack_start(self.lbl_nozzle, False, False, 0)
        # Ana yatay bÃ¶lÃ¼cÃ¼ (TX | RX/PARSED)
        self.paned_main = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        outer.pack_start(self.paned_main, True, True, 0)

        # TX paneli
        self.tv_tx = self._mk_textview()
        box_tx = self._build_labeled_view("TX — Komutlar & Seri Port", self.tv_tx)
        self.paned_main.add1(box_tx)

        # SaÄŸ tarafta RX Ã¼stte, PARSED altta (dikey bÃ¶lÃ¼cÃ¼)
        self.paned_right = Gtk.Paned(orientation=Gtk.Orientation.VERTICAL)

        self.tv_rx = self._mk_textview()
        box_rx = self._build_labeled_view("RX — Ham Gelen Frame", self.tv_rx)
        self.paned_right.add1(box_rx)

        self.tv_parsed = self._mk_textview()
        box_pr = self._build_labeled_view("PARSE — Çözülmüş Durum / Hacim / Tutar", self.tv_parsed)
        self.paned_right.add2(box_pr)

        self.paned_main.add2(self.paned_right)
        # --- NEW: State/LED & Help ---
        self._state = "RESET"          # last known pump state
        self._state_led = "âšª"          # white (RESET)
        self._refresh_title()           # put LED + state into window title
        # keyboard shortcuts: F1=Help, F2=toggle quick status toast
        self.connect("key-press-event", self._on_key_press)
        self._status_toast = None       # lazy-create on first F2
        # F1â€™in TextView odaklarÄ±nda da Ã§alÄ±ÅŸmasÄ± iÃ§in aynÄ± handlerâ€™Ä± Ã¼Ã§ TVâ€™ye de baÄŸla
        for _tv in (self.tv_tx, self.tv_rx, self.tv_parsed):
            _tv.connect("key-press-event", self._on_key_press)
        # Public hook for RX parser: call with a canonical state string
        # e.g. self.on_pump_status("FILLING")
        # Canonical set: RESET, AUTHORIZED, FILLING, SUSPENDED, FILLING COMPLETED,
        # MAX AMOUNT/VOLUME, NOT PROGRAMMED, SWITCHED OFF

        self._attach_tv_context(self.tv_tx,  allow_clear=True)
        self._attach_tv_context(self.tv_rx,  allow_clear=True)
        self._attach_tv_context(self.tv_parsed, allow_clear=True)
        # --- CRC sıralama seçeneği + DCC butonları
        crcrow = Gtk.Box(spacing=6)
        outer.pack_start(crcrow, False, False, 0)
        crcrow.pack_start(Gtk.Label(label="CRC order:"), False, False, 0)
        self.cmb_crc = Gtk.ComboBoxText()
        self.cmb_crc.append_text("LO,HI")
        self.cmb_crc.append_text("HI,LO")
        self.cmb_crc.set_active(0)  # default LO,HI
        crcrow.pack_start(self.cmb_crc, False, False, 0)

        # Auto ACK: DC/CD (uzun) cevap çerçevelerinde isteğe bağlı 0x50 0xC0 0xFA gönderimi.
        # Not: Mepsan satış logunda MIN-POLL (50 20 FA) için controller'dan ek ACK yoktur;
        # bu yüzden kısa 50 20/50 70 FA çerçevelerinde artık otomatik ACK kullanılmaz. :contentReference[oaicite:3]{index=3}
        self.chk_auto_ack = Gtk.CheckButton(label="Auto ACK")
        self.chk_auto_ack.set_active(True)  # varsayılan: açık
        self.chk_auto_ack.set_tooltip_text(
            "DC/CD (uzun) cevap çerçevelerinde isteğe bağlı 0x50 0xC0 0xFA gönder"
        )
        crcrow.pack_start(self.chk_auto_ack, False, False, 0)
        # Auto POLL: Heartbeat için periyodik MIN-POLL (50 20 FA)
        self.chk_auto_poll = Gtk.CheckButton(label="Auto POLL")
        self.chk_auto_poll.set_active(False)  # varsayılan: kapalı
        self.chk_auto_poll.set_tooltip_text(
            "500 ms periyotla 50 20 FA gönder; bağlantı canlılığını kontrol et"
        )
        self.chk_auto_poll.connect("toggled", self.on_auto_poll_toggled)
        crcrow.pack_start(self.chk_auto_poll, False, False, 0)

        btns = Gtk.Box(spacing=6)
        # DCC action butonlarını liste olarak tutalım ki HS'ye göre topluca enable/disable edebilelim
        btns = Gtk.Box(spacing=6)
        # DCC action butonlarını liste olarak tutalım ki HS'ye göre topluca enable/disable edebilelim
        self._dcc_buttons: list[Gtk.Button] = []
        for text, dcc in [
            ("RETURN_STATUS",0x00),
            ("RETURN_FILL_INFO",0x04),
            ("AUTHORIZE",0x06),
            ("RESUME/START",0x0C),
            ("PAUSE",0x0B),
            ("STOP",0x08),
            ("SWITCH_OFF",0x0A),
        ]:
            b = Gtk.Button(label=text)
            b.connect("clicked", self.on_send_dcc, dcc)
            btns.pack_start(b, False, False, 0)
            self._dcc_buttons.append(b)

        # -- Yeni: Min POLL / Min ACK (YAT ile aynı kısa çerçeveler) --
        btn_poll_min = Gtk.Button(label="POLL (MIN)")
        btn_poll_min.set_tooltip_text("0x50 0x20 0xFA gönder")
        btn_poll_min.connect("clicked", lambda *_: self._send_min_poll())
        btns.pack_start(btn_poll_min, False, False, 0)

        btn_ack_min = Gtk.Button(label="ACK (MIN)")
        btn_ack_min.set_tooltip_text("0x50 0xC0 0xFA gönder")
        btn_ack_min.connect("clicked", lambda *_: self._send_min_ack())
        btns.pack_start(btn_ack_min, False, False, 0)

        # Volume Total Counters (CD101 / DC101) isteği için kısayol
        btn_totals = Gtk.Button(label="TOTAL (CD101)")
        btn_totals.set_tooltip_text("CD101: Volume Total Counters isteği gönder")
        btn_totals.connect("clicked", self.on_request_total_counters)
        btns.pack_start(btn_totals, False, False, 0)

        # Sağ tarafa "Yardım (F1)" butonu
        btns.pack_start(Gtk.Box(), True, True, 0)  # esnek boÅŸluk: butonu saÄŸa iter
        b_help = Gtk.Button(label="Yardım (F1)")
        b_help.set_tooltip_text("Durum makinesi ve komut özeti")
        b_help.connect("clicked", lambda *_: self._show_rich_help())
        btns.pack_start(b_help, False, False, 0)
        outer.pack_start(btns, False, False, 0)

        # --- Yetkili Dolum paneli (tek buton, 2 mod) -------------------------
        auth = Gtk.Box(spacing=6)
        outer.pack_start(auth, False, False, 0)
        # Mod seçici
        self.cmb_mode = Gtk.ComboBoxText()
        self.cmb_mode.append_text("Mod-A: Limitli (L)")
        self.cmb_mode.append_text("Mod-B: Serbest")
        self.cmb_mode.set_active(0)
        auth.pack_start(Gtk.Label(label="Yetkili Dolum:"), False, False, 0)
        auth.pack_start(self.cmb_mode, False, False, 0)
        # Limit (L)
        self.spn_limit_l = Gtk.SpinButton.new_with_range(0.1, 9999.0, 0.1)
        self.spn_limit_l.set_range(0.1, 250.0)
        self.spn_limit_l.set_increments(0.1, 1.0)
        self.spn_limit_l.set_value(2.0)
        auth.pack_start(Gtk.Label(label="Limit (L):"), False, False, 0)
        auth.pack_start(self.spn_limit_l, False, False, 0)
        # Başlat butonu
        self.btn_start_auth = Gtk.Button(label="Start Authorized")
        self.btn_start_auth.set_tooltip_text("AUTHORIZE gönderir; Mod-A'da limit dolunca STOP")
        self.btn_start_auth.connect("clicked", self.on_start_authorized)
        # Esnek boşluk + buton sağa
        auth.pack_start(Gtk.Box(), True, True, 0)
        auth.pack_start(self.btn_start_auth, False, False, 0)

        # Preset izleyici: Mod-A için hedef ml ve tek-sefer STOP kilidi
        self._preset_target_ml = None
        self._preset_stop_sent = False

        # CD3 sonrası, ilk MIN-BUSY'de tek-seferlik AUTHORIZE gönderimi için bayrak
        self._auth_pending_after_preset = False

        # tek seferlik otomatik AUTHORIZE tekrarına izin verir.
        self._auth_pending_for_nozzle = False


        # Satış takibi (Mepsan protokolüne göre):
        #  - _sale_active: AUTHORIZED/FILLING/COMPLETE penceresinde miyiz?
        #  - _sale_has_dc2: bu satışta en az bir DC2 gördük mü?
        #  - _sale_last_*   : son DC2’nin ham ve birim karşılığı
        #  - _last_nozzle_logged: GunOn/GunOff CSV logunda tek-olay geçiş koruması
        self._sale_active = False
        self._sale_has_dc2 = False
        self._sale_last_vol_raw = None
        self._sale_last_amo_raw = None
        self._sale_last_vol_l = None
        self._sale_last_amo_unit = None
        self._last_nozzle_logged = None
        # durum
        self.ser = None
        self.reader = None
        self.rxq = queue.Queue()

        GLib.timeout_add(50, self._poll_rx)
        self.connect("destroy", self.on_destroy)
        # --- Handshake (ilk hazır olma) + Heartbeat ---
        self._hs_ok = False          # DC1/DC3 görülürse True
        self._hb_timer_id = None
        self._hb_wait_logged = False   # HS yokken HB bekliyor mesajını sadece 1 ke
        self._hb_interval_ms = 500    # 0.5 s — saha isteği: POLL periyodu
        self._hb_last_activity = 0.0

        # Başlangıçta HS yok → komut butonlarını kilitle
        self._set_controls_enabled(False)
        # Başlangıçta HS yok → komut butonlarını kilitle
        self._set_controls_enabled(False)
        def _fix_paned_positions():
            try:
                self.paned_main.set_position(400)
            except Exception:
                pass
            try:
                self.paned_right.set_position(240)
            except Exception:
                pass
            # Tek sefer Ã§alÄ±ÅŸsÄ±n
            return False
        # Açılışta sürüm bilgisini PARSE paneline yaz
        self.append_tv(self.tv_parsed, f"[APP] Controller ver={APP_VERSION}")
        # İlk açılış mesajı (HS bekleniyor)
        self._set_msg("HABERLEŞME YOK")
        # Sürüm/Tarih/Saat ve varsayılan görseller
        try:
            if self.ui.get("lblvers"): self.ui["lblvers"].set_text(APP_VERSION)
            # tarih/saat canlı güncelleme
            GLib.timeout_add_seconds(1, self._tick_clock)
           # araç ikonu varsayılan OFF
            self._set_vehicle_icon(on=False)
        except Exception:
            pass
        # IP bilgileri (varsa)
        try:
            eth_ip, wifi_ip = self._detect_ips()
            if getattr(self, "lbl_eth_ip", None):
                self.lbl_eth_ip.set_text(eth_ip or "---.---.---.---")
            if getattr(self, "lbl_wifi_ip", None):
                self.lbl_wifi_ip.set_text(wifi_ip or "---.---.---.---")
        except Exception:
            pass
        # Kalıcı sayaçları yükle ve ekrana yansıt (AÇILIŞTA)
        try:
            self._load_persist()
            if getattr(self, "lbl_vechs", None):
                self.lbl_vechs.set_text(str(int(self._persist.get("vechs", 0))))
            # lblcounter'ı tek yardımcıyla güncelle
            self._update_lblcounter_display(self._persist.get("total_l", 0.0), "startup")
            self._dbg("persist applied to GUI at startup")
            # UI başka akışla üstünü yazarsa, idle'da bir kez daha bastır
            def _reapply():
                try:
                    self._update_lblcounter_display(self._persist.get("total_l", 0.0), "idle reapply")
                except Exception:
                    pass
                return False
            GLib.idle_add(_reapply)
        except Exception:
            pass
            # UI farklı bir akışta üstüne yazmışsa, idle'da bir kez daha helper ile bastır.
            try:
                def _reapply():
                    try:
                        self._update_lblcounter_display(self._persist.get("total_l", 0.0), "idle reapply")
                    except Exception:
                        pass
                    return False
                GLib.idle_add(_reapply)
            except Exception:
                pass
        # Açılışta varsayılanlarla bağlan ve Auto POLL'i başlat
        try:
            GLib.idle_add(self._startup_auto_open)
        except Exception:
            pass

    # --- Yetkili Dolum başlatıcı ------------------------------------------------
    def on_start_authorized(self, *_):
        # 1) Seri port açık mı?
        if not (self.ser and self.ser.is_open):
            self.append_tv(self.tv_tx, "[ERR] Serial not open")
            return
        # 2) Handshake tamam mı? (DC1/DC3/CD1 görülmüş olmalı)
        if not getattr(self, "_hs_ok", False):
            self.append_tv(
                self.tv_tx,
                "[SAFE] HS yokken AUTHORIZE/START gönderilmez — önce Auto POLL ile pompanın cevap verdiğini görün."
            )
            return

        mode = self.cmb_mode.get_active()

        # --- Mod hazırlığı (Mod-A: preset volume, Mod-B: serbest) ---
        if mode == 0:
            # Mod-A: Limitli (L) — hem pompa tarafına CD3 göndereceğiz,
            # hem de yerel olarak STOP için hedef ml izleyeceğiz.
            try:
                liters = float(self.spn_limit_l.get_value())
            except Exception:
                liters = 0.0

            # Kullanıcı limiti: 0.1 .. 250.0 L
            if liters < 0.1:
                liters = 0.1
            if liters > 250.0:
                liters = 250.0

            # Yerel preset hedefi (ml)
            self._preset_target_ml = int(round(liters * 1000.0))
            self._preset_stop_sent = False
            self.append_tv(
                self.tv_tx,
                f"[LOCAL] PRESET hedef={self._preset_target_ml} ml (Mod-A, {liters:.2f} L)"
            )

            # 3A) Önce CD3 – Preset Volume gönder
            try:
                # CD3'e doğrudan litre cinsinden (float) veriyoruz;
                # fonksiyon içinde protokol ölçeğine (x100) çevrilecek.
                self._send_cd3_preset_volume(liters)
            except Exception as e:
                self.append_tv(self.tv_tx, f"[TX-ERR] CD3 PRESET: {e}")
                return
        else:
            # Mod-B: limitsiz
            self._preset_target_ml = None
            self._preset_stop_sent = False
            self.append_tv(self.tv_tx, "[LOCAL] PRESET kapalı (Mod-B)")

        # 3B) AUTHORIZE gönderimi — el sıkışma tabanlı: CD3 sonrası ilk MIN-BUSY'de
        try:
            self._auth_pending_after_preset = True
            self.append_tv(
                self.tv_tx,
                "[AUTH] AUTHORIZE pending — will send after next MIN-BUSY"
            )
        except Exception:
            self._auth_pending_after_preset = True
    # --- Komut butonlarını HS durumuna göre aç/kapat ---
    def _set_controls_enabled(self, enabled: bool):
        try:
            self.btn_start_auth.set_sensitive(enabled)
            for b in getattr(self, "_dcc_buttons", []):
                # RETURN_STATUS (0x00) ve RETURN_FILL_INFO (0x04) HS öncesi de serbest kalabilir
                label = b.get_label() or ""
                if label in ("RETURN_STATUS", "RETURN_FILL_INFO"):
                    b.set_sensitive(True)
                else:
                    b.set_sensitive(enabled)
        except Exception:
           pass

    # ---- DOSYA LOG yardımcıları ----
    def _open_log(self):
        """
        .\\logs\\controller_TXRX_YYYYMMDD_HHMMSS.log dosyasını aç.
        GUI donsa bile satırlar dosyaya düşsün.
        """
        try:
            logdir = pathlib.Path(".") / "logs"
            logdir.mkdir(parents=True, exist_ok=True)
            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            fp = logdir / f"controller_TXRX_{ts}.log"
            f = open(fp, "a", encoding="utf-8", buffering=1)  # line buffered
            # Başlık: sürüm + BASE meta
            base_sha, base_lines = "unknown", "unknown"
            try:
                with open(__file__, "r", encoding="utf-8", errors="ignore") as _sf:
                    # İlk birkaç satırda BASE meta bekleniyor
                    for _ in range(4):
                        _ln = _sf.readline()
                        if not _ln:
                            break
                        _lns = _ln.strip()
                        if _lns.startswith("# BASE_SHA="):
                            base_sha = _lns.split("=", 1)[1].strip()
                        elif _lns.startswith("# BASE_LINES="):
                            base_lines = _lns.split("=", 1)[1].strip()
            except Exception:
                pass
            hdr = [
                "**********************",
                "Controller logfile start",
                f"Controller: {APP_TITLE}",  # sürüm bilgisi (APP_VERSION) başlığa eklendi
                f"BASE_SHA: {base_sha}",
                f"BASE_LINES: {base_lines}",                
                f"Path: {fp}",
                f"Start time: {ts}",
                "**********************",
            ]
            for line in hdr:
                f.write(line + "\n")
            return f
        except Exception:
            # Son çare: yazma devnull
            try:
                return open(os.devnull, "w")
            except Exception:
                return None

    def _log(self, line: str):
        """Disk log dosyasına zaman damgalı satır yazar."""
        try:
            if self._logf:
                ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                self._logf.write(f"{ts} {line}\n")
                self._logf.flush()
        except Exception:
            # Log yazarken hata olursa uygulamayı bozmayalım
            pass
    def _log_csv_event(self, logCode: str, fuel: str = ""):
        """
        logs.CsvLogger varsa standart kolonlarla tek satır ekler.
        logCode: AuthOk | NoAuth | GunOn | GunOff | FillOk (FillOk zaten başka yerde yazılıyor)
        fuel    : "x.xx" (opsiyonel, çoğu event için boş bırakılır)
        """
        try:
            if getattr(self, "_logger", None):
                self._logger.append(
                    rfid       = getattr(self, "last_rfid", ""),
                    firstName  = getattr(self, "last_user_first", ""),
                    lastName   = getattr(self, "last_user_last", ""),
                    plate      = getattr(self, "last_user_plate", ""),
                    limit_val  = getattr(self, "last_user_limit", ""),
                    fuel       = fuel,
                    logCode    = logCode,
                    sendOk     = "NA",
                )
        except Exception:
            pass

    def _on_serial_raw(self, data: bytes):
        """
        SerialReader'ın okuduğu ham chunk'ları debug amaçlı log dosyasına yazar.
        GUI'yi şişirmemek için sadece _log kullanılır.

        Özellikle her chunk içindeki 0x50 ... 0xFA segmentlerini hexdump ediyoruz:
          [SER-RAW-SEG] 50360208...03FA
        """
        try:
            # Önce chunk'ın özetini yaz (uzunsa kes)
            hx = hexline(data)
            if len(hx) > 96:
                hx_short = hx[:96] + "..."
            else:
                hx_short = hx
            self._log(f"[SER-RAW] chunk len={len(data)} {hx_short}")

            # Sonra chunk içinde geçen tüm 0x50 ... 0xFA segmentlerini ayrı ayrı logla
            i = 0
            mv = memoryview(data)
            while True:
                try:
                    start = data.index(0x50, i)
                except ValueError:
                    break
                try:
                    end = data.index(0xFA, start + 1)
                except ValueError:
                    # Bu chunk içinde FA yok; bir sonraki chunk ile birleşmesini bekleyeceğiz
                    break
                seg = bytes(mv[start:end+1])
                try:
                    seg_hex = hexline(seg)
                except Exception:
                    seg_hex = seg.hex().upper()
                self._log(f"[SER-RAW-SEG] {seg_hex}")
                i = end + 1
        except Exception:
            # Debug log hataları görmezden gel
            pass
    # ---- Yeni: seri port hatasÄ±nÄ± GUI'de gÃ¶stermek iÃ§in kÃ¼Ã§Ã¼k yardÄ±mcÄ± ----
    def _show_serial_error_dialog(self, msg: str):
        """
        Seri port erişimiyle ilgili kritik hata durumlarında (Access is denied,
        ClearCommError, timeout vb.) kullanıcıya gözle görülür bir uyarı penceresi aç.
        Bu, log satırını kaçırmayı önler.
        """
        dlg = Gtk.MessageDialog(
            transient_for=self,
            flags=0,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text="Seri Port HatasÄ±",
        )
        # KullanÄ±cÄ±ya tipik nedenleri de hatÄ±rlat:
        detail = (
            f"{msg}\n\n"
            "Olası nedenler:\n"
            "• Bu COM port başka bir uygulama tarafından kullanılıyor olabilir\n"
            "• Yanlış COM seçilmiş olabilir\n"
            "• Windows erişimi engelledi (izin/yetki)\n"
        )
        dlg.format_secondary_text(detail)
        dlg.set_modal(True)
        dlg.run()
        dlg.destroy()

    # ---------- NEW: Status/LED ----------
    def _refresh_title(self):
        self.set_title(f"{self._state_led}  {self._state} — {APP_TITLE}")
    # YENİ: Görsel durumu (başlık/LED/etiket) güncelleyen yardımcı.
    # Not: PARSE paneline satır DÜŞMEZ; sadece görsel öğeler güncellenir.
    def _apply_visual_state(self, state: str):
        st = (state or "").upper().strip() or "RESET"
        led = {
            "NOT PROGRAMMED": "🔴",
            "AUTHORIZED":     "🟠",
            "FILLING":        "🟢",
            "SUSPENDED":      "🟡",
            "FILLING COMPLETED": "🔵",
            "MAX AMOUNT/VOLUME": "🟣",
            "SWITCHED OFF":   "⚫",
            "RESET":          "⚪",
        }.get(st, "⚪")
        self._state, self._state_led = st, led
        self._refresh_title()
        rgb = {
            "NOT PROGRAMMED": (0.95, 0.15, 0.15),
            "AUTHORIZED":     (0.95, 0.55, 0.10),
            "FILLING":        (0.10, 0.75, 0.25),
            "SUSPENDED":      (0.95, 0.85, 0.10),
            "FILLING COMPLETED": (0.20, 0.45, 0.95),
            "MAX AMOUNT/VOLUME": (0.60, 0.30, 0.80),
            "SWITCHED OFF":   (0.10, 0.10, 0.10),
            "RESET":          (0.75, 0.75, 0.75),
       }.get(self._state, (0.75, 0.75, 0.75))
        self._led_color = rgb
        try:
            self.lbl_state.set_text(self._state)
            self.led.set_tooltip_text(self._state)
            self.led.queue_draw()
        except Exception:
           pass
    def _update_nozzle_icon(self):
        """
        lbl_nozzle metni 'NOZZLE: IN' / 'NOZZLE: OUT' ise, Glade'deki imggun'ı günceller.
        lbl_nozzle Python tarafında güncellenmeye devam eder; burada sadece görsel eşlik eder.
        """
        try:
            if self.imggun is None:
                return
            # lbl_nozzle mevcutsa metnini okuyalım:
            txt = ""
            try:
                txt = self.lbl_nozzle.get_text()
            except Exception:
                txt = ""
            is_out = ("OUT" in (txt or "").upper())
            # ikon dosyaları /gui/resources/ altında olmalı
            icon = "gun_pump_on_64x64.png" if is_out else "gun_pump_off_64x64.png"
            p = (RES_DIR / icon)
            if p.exists():
                self.imggun.set_from_file(str(p))
            if p.exists():
                self.imggun.set_from_file(str(p))
            # CSV log: GunOn / GunOff (idempotent, tek-olay geçiş koruması)
            try:
                code = "GunOn" if is_out else "GunOff"
                last = getattr(self, "_last_nozzle_logged", None)
                # Aynı durumda tekrar tekrar DC1 geldiğinde log spam'ini engelle:
                # sadece IN↔OUT geçişinde tek satır düş.
                if code != last:
                    if code == "GunOn":
                        # GunOn: yalnızca satış penceresi açıksa (AUTHORIZED/FILLING/COMPLETE) yaz.
                        if getattr(self, "_sale_active", False):
                            self._log_csv_event("GunOn")
                            self._last_nozzle_logged = "GunOn"
                    else:
                        # GunOff: ancak daha önce GunOn loglandıysa bir defa yaz.
                        if last == "GunOn":
                            self._log_csv_event("GunOff")
                        self._last_nozzle_logged = "GunOff"
            except Exception:
                pass
        except Exception:
            pass

    def _update_station_icon(self, state: str):
        """
        lbl_dc1 (state) parse edildiğinde imgpump ikonunu günceller:
        - FILLING            -> station_on_64x64.png
        - FILLING COMPLETED  -> station_off_64x64.png
        - SUSPENDED          -> station_suspend_64x64.png
        Diğer durumlarda ikon değiştirilmez.
        """
        try:
            if self.imgpump is None:
                return
            s = (state or "").upper()
            fname = None
            if "FILLING COMPLETED" in s:
                fname = "station_off_64x64.png"
            elif "FILLING" in s:
                fname = "station_on_64x64.png"
            elif "SUSPENDED" in s:
                fname = "station_suspend_64x64.png"
            if fname:
                p = (RES_DIR / fname)
                if p.exists():
                    self.imgpump.set_from_file(str(p))
        except Exception:
            pass

    # ---- UI durum mesajı (lblmsg) için küçük yardımcı ----
    def _set_msg(self, text: str):
        try:
            if getattr(self, "lbl_msg", None):
                self.lbl_msg.set_text(text)
        except Exception:
            pass

    # --- Saat/Tarih etiketi ---
    def _tick_clock(self):
        try:
            now = datetime.datetime.now()
            if self.ui.get("lbldate"): self.ui["lbldate"].set_text(now.strftime("%d.%m.%Y"))
            if self.ui.get("lbltime"): self.ui["lbltime"].set_text(now.strftime("%H:%M:%S"))
        except Exception:
            pass
        return True

    # --- Araç ikonu (imgvhec): AUTHORIZED/aktif satışta ON, aksi halde OFF ---
    def _set_vehicle_icon(self, on: bool):
        try:
            if getattr(self, "imgvhec", None) is None: return
            fname = "truck_mix_ON_48x48.png" if on else "truck_mix_OFF_48x48.png"
            p = (RES_DIR / fname)
            if p.exists():
                self.imgvhec.set_from_file(str(p))
        except Exception:
            pass

    # --- IP tespiti (Windows ipconfig ayıklama; yoksa socket fallback) ---
    def _detect_ips(self):
        eth_ip, wifi_ip = None, None
        try:
            out = os.popen("ipconfig").read()
            sect = None
            for line in out.splitlines():
                ls = line.strip()
                if "Ethernet adapter" in ls:
                    sect = "eth"
                elif "Wireless LAN adapter" in ls or "Wi-Fi" in ls:
                    sect = "wifi"
                if ls.startswith("IPv4 Address") or ls.startswith("IPv4 Adres"):
                    ip = ls.split(":")[-1].strip()
                    if sect == "eth" and not eth_ip: eth_ip = ip
                    if sect == "wifi" and not wifi_ip: wifi_ip = ip
        except Exception:
            pass
        # Fallback: tek IP bulursak ikisine de yaz
        if not (eth_ip or wifi_ip):
            try:
                import socket
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.connect(("8.8.8.8", 80))
                ip = s.getsockname()[0]; s.close()
                eth_ip = eth_ip or ip; wifi_ip = wifi_ip or ip
            except Exception:
                pass
        return eth_ip, wifi_ip

   # --- Bağlantı ikonlarını periyodik güncelle ---
    def _update_conn_icons(self):
        """
        imggps (RS-485), imglan (Ethernet), imgwifi (Wi-Fi), imggsm (GPRS)
        ikonlarını mevcut duruma göre günceller.
        Kaynak ikonlar: /gui/resources/
          - RS485_on_48x48.png / RS485_off_48x48.png
          - lan_on_48x48.png   / lan_off_48x48.png
          - wifi_on_48x48.png  / wifi_off_48x48.png
          - gsm_on_48x48.png   / gsm_off_48x48.png
        """
        try:
            # 1) RS-485 (com port)
            img = self.ui.get("imggps")
            if img is not None:
                is_open = bool(getattr(self, "ser", None) and getattr(self.ser, "is_open", False))
                fn = "Rs485_on_48x48.png" if is_open else "Rs485_off_48x48.png"
                img.set_from_file(str(RES_DIR / fn))
        except Exception as e:
            try:
                self._log(f"[CONN-ERR] RS485 icon update failed: {e}")
            except Exception:
                pass
        try:
            # 2) LAN/Wi-Fi: IP’ler varsa "on", yoksa "off"
            eth_ip, wifi_ip = self._detect_ips()
            if getattr(self, "lbl_eth_ip", None):
                self.lbl_eth_ip.set_text(eth_ip or "---.---.---.---")
            if getattr(self, "lbl_wifi_ip", None):
                self.lbl_wifi_ip.set_text(wifi_ip or "---.---.---.---")
            img = self.ui.get("imglan")
            if img is not None:
                img.set_from_file(str(RES_DIR / ("lan_on_48x48.png" if eth_ip else "lan_off_48x48.png")))
            img = self.ui.get("imgwifi")
            if img is not None:
                img.set_from_file(str(RES_DIR / ("wifi_on_48x48.png" if wifi_ip else "wifi_off_48x48.png")))
        except Exception as e:
            try:
                self._log(f"[CONN-ERR] LAN/WiFi icon update failed: {e}")
            except Exception:
                pass
        try:
            # 3) GPRS: Şimdilik durum bilgimiz yok → varsayılan 'off'
            img = self.ui.get("imggsm")
            if img is not None:
                img.set_from_file(str(RES_DIR / "gsm_off_48x48.png"))
        except Exception as e:
            try:
                self._log(f"[CONN-ERR] GPRS icon update failed: {e}")
            except Exception:
                pass

    def _update_conn_icons_timer(self):
        try:
            self._update_conn_icons()
        except Exception as e:
            try:
                self._log(f"[CONN-ERR] conn icons timer failed: {e}")
            except Exception:
                pass
        return True  # periyodik devam
    # --- Kalıcı sayaçlar: lblvechs (adet) + lblcounter (toplam litre) ---
    def _persist_path(self):
        d = BASE_DIR / "data"
        try:
            d.mkdir(parents=True, exist_ok=True)
        except Exception:
            pass
        p = d / "counters.json"
        try:
            sys.stderr.write(f"[PERSIST-PATH] {p}\n")
        except Exception:
            pass
        return p

    def _load_persist(self):
        """Diskteki sayaçları yükle; yoksa başlangıç değerleriyle başlat."""
        self._persist = {"vechs": 0, "total_l": 0.0}
        p = self._persist_path()
        try:
            if p.exists():
                import json
                obj = json.loads(p.read_text(encoding="utf-8"))
                self._persist["vechs"]   = int(obj.get("vechs", 0))
                self._persist["total_l"] = float(obj.get("total_l", 0.0))
            else:
                self._dbg("persist file not found, starting at zeros")
        except Exception as e:
            self._dbg(f"persist load err: {e}")

    def _save_persist(self):
        """Sayaçları diske yazar (atomic yazım denemesi)."""
        try:
            import json, tempfile
            p = self._persist_path()
            tmp = tempfile.NamedTemporaryFile("w", delete=False, encoding="utf-8", dir=str(p.parent))
            json.dump(self._persist, tmp); tmp.flush(); tmp.close()
            os.replace(tmp.name, str(p))
        except Exception as e:
            self._dbg(f"persist save err: {e}")
    # --- lblcounter tek noktadan güncellensin (ana thread) ---
    def _update_lblcounter_display(self, liters: float, reason: str = ""):
        try:
            self._persist["total_l"] = float(liters)
        except Exception:
            pass
        # Sadece Glade lblcounter'ı güncelle (DC101 ile izole)
        lbl = getattr(self, "lblcounter", None)
        if lbl is not None:
            def _apply():
                try:
                    # Sade sayı (istersen " L" ekleyebilirsin)
                    lbl.set_text(f"{self._persist['total_l']:.2f}")
                except Exception:
                    pass
                return False
        try:
            GLib.idle_add(_apply)
        except Exception:
            _apply()

    # --- lblcounter tek noktadan güncellensin (ana thread) ---
    def _update_lblcounter_display(self, liters: float, reason: str = ""):
        try:
            self._persist["total_l"] = float(liters)
        except Exception:
            pass
        lbl = getattr(self, "lblcounter", None)
        if lbl is not None:
            def _apply():
                try:
                    # İstenen biçim: düz sayı
                    lbl.set_text(f"{self._persist['total_l']:.2f}")
                except Exception:
                    pass
                return False
            try:
                GLib.idle_add(_apply)
            except Exception:
                _apply()

    # --- ORTAK: satış sonunda sayacı artır ve GUI'yi güncelle ---
    def _bump_counters_once(self, liters: float):
        """
        Her satışın terminal anında tam **bir kez** çağrılmalı.
        - self._persist['vechs']   += 1
        - self._persist['total_l'] += liters
        - data/counters.json'a yaz
        - lblvechs ve lblcounter (mapping: self.lbl_dc101) ekrana bas
        """
        try:
            if getattr(self, "_counters_bumped_for_this_sale", False):
                self._dbg("bump skipped (already bumped for this sale)")
                return  # tek sefer koruması
            try:
                l = float(liters)
            except Exception:
                l = 0.0
            if not hasattr(self, "_persist") or not isinstance(self._persist, dict):
                self._load_persist()
            self._persist["vechs"]   = int(self._persist.get("vechs", 0)) + 1
            self._persist["total_l"] = float(self._persist.get("total_l", 0.0)) + float(l)
            self._save_persist()
            # GUI'ye tek kanaldan yaz
            try:
                if getattr(self, "lbl_vechs", None):
                    self.lbl_vechs.set_text(str(int(self._persist["vechs"])))
            except Exception:
                pass
            self._update_lblcounter_display(self._persist.get("total_l", 0.0), "bump")
            # GUI
            try:
                if getattr(self, "lbl_vechs", None):
                    self.lbl_vechs.set_text(str(int(self._persist["vechs"])))
            except Exception:
                pass
            try:
                # Glade 'lblcounter' doğrudan kalıcı toplamla güncellenir (DC101’den bağımsız)
                if getattr(self, "lblcounter", None):
                    v = float(self._persist['total_l'])
                    self.lblcounter.set_text(f"{v:.2f}")
            except Exception:
                pass
            self._counters_bumped_for_this_sale = True            
        except Exception:
            pass
    # ---- HOME alanı için sabit arka plan rengi (yalnız boxhome; label'ları etkilemez) ----
    def _apply_homebox_bg(self, hex_color: str):
        """
        'boxhome' widget'ının arka planını doğrudan boyar.
       Yalnız bu widget etkilenir; altındaki label buton vb. CSS/tema değerleri korunur.
        """
        try:
            box = None
            if hasattr(self, "ui"):
                box = self.ui.get("boxhome")
            if box is None and getattr(self, "_builder", None):
                box = self._builder.get_object("boxhome")
            if box is None:
                sys.stderr.write("[HOME-BG-WARN] boxhome not found\n")
                return
            rgba = Gdk.RGBA()
            if not rgba.parse(hex_color):
                rgba.parse("#1e293b")
            # Sadece bu widget'ın arka planını boya (GTK3)
            box.override_background_color(Gtk.StateFlags.NORMAL, rgba)
            sys.stderr.write(f"[HOME-BG] painted background={rgba.to_string()} on boxhome\n")
        except Exception as e:
            sys.stderr.write(f"[HOME-BG-ERR] {e}\n")
    # ---------------------------------------------------------------------
    # AŞAMA-1: Glade/CSS yüklemesi ve Glade’de VAR OLAN widget’lar için mapping
    # ---------------------------------------------------------------------
    def _init_glade_and_css(self):
        """
        /gui/MainWindow.glade ve /gui/resources/style.css dosyalarını yükler.
        - CSS sağlayıcıyı ekler (url(...) yollarını mutlaklaştırarak)
        - YALNIZ Glade’de mevcut olan widget’ları self.ui[...] sözlüğüne koyar.
        - TX/RX/PARSED bölümleri BU AŞAMADA Glade’e taşınmaz.
        """
        # Çoklu arama yolu: 1) …/gui  2) …/ (aynı klasör)  3) PUMP_GUI_DIR
        cand_dirs = [GUI_DIR, BASE_DIR]
        try:
            env_dir = os.environ.get("PUMP_GUI_DIR", "").strip()
            if env_dir:
                from pathlib import Path as _P
                cand_dirs.insert(0, _P(env_dir))
        except Exception:
            pass
        glade_file = None
        css_file = None
        for d in cand_dirs:
            g = d / "MainWindow.glade"
            s = d / "style.css"
            if g.exists() and g.is_file():
                glade_file = g
            if s.exists() and s.is_file():
                css_file = s
            if glade_file and css_file:
                break
        if not glade_file:
            sys.stderr.write("[GLADE-INIT-WARN] MainWindow.glade not found in "
                             + ", ".join(str(p) for p in cand_dirs) + "\n")
        if not css_file:
            sys.stderr.write("[CSS-WARN] style.css not found in "
                             + ", ".join(str(p) for p in cand_dirs) + "\n")

        # 1) CSS: varsa yükle ve uygula
        if css_file and css_file.exists():
            css_text = _read_text(css_file)
            if css_text:
                css_text = _css_with_absolute_urls(css_text, RES_DIR)
                provider = Gtk.CssProvider()
                try:
                    provider.load_from_data(css_text.encode("utf-8"))
                    Gtk.StyleContext.add_provider_for_screen(
                        Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
                    )
                    sys.stderr.write(f"[CSS-OK] applied: {css_file}\n")
                except Exception as e:
                    sys.stderr.write(f"[CSS-WARN] {e}\n")

        # 2) Glade: varsa yükle, sinyalleri bağlamaya çalış
        self._builder = None
        if glade_file and glade_file.exists():
            b = Gtk.Builder()
            b.add_from_file(str(glade_file))
            try:
                b.connect_signals(self)  # Glade’de handler adları varsa eşleşir
            except Exception:
                pass
            self._builder = b
            try:
                sys.stderr.write(f"[GLADE-OK] loaded: {glade_file}\n")
            except Exception:
                pass
            # --- Glade mapping (yalnız istenen ögeler) ---
            # Labels:
            try:
                _lbldurum   = b.get_object("lbldurum")     # lbl_dc1 + lbl_state
                _lbllevel   = b.get_object("lbllevel")     # lbl_dc3
                _lblcounter = b.get_object("lblcounter")   # TOPLAM sayacı (persist'ten)
                _lblmsg     = b.get_object("lblmsg")       # durum mesajı
                _lbluserid  = b.get_object("lbluserid")
                _lastfuel   = b.get_object("lastfuel")
                _lblvechs   = b.get_object("lblvechs")
                _lblEthIP   = b.get_object("lblEthIP")
                _lblWiFiIP  = b.get_object("lblWiFiIP")
                if _lbldurum is not None:
                    self.lbl_dc1   = _lbldurum
                    self.lbl_state = _lbldurum
                if _lbllevel is not None:
                    self.lbl_dc3   = _lbllevel
                    # Glade'deki büyük seviye alanı: DC2 litreyi yalın göstereceğiz
                    self._glade_level_label = _lbllevel
                if _lblcounter is not None:
                    # DC101'dan AYRI: Bu glade alanı kalıcı toplam litre sayacını gösterir.
                    # (Yani counters.json’dan okunur/yazılır; DC101 ile hiç ilişki yok.)
                    self.lblcounter = _lblcounter
                if _lblmsg is not None:
                    self.lbl_msg = _lblmsg
                if _lbluserid is not None:
                    self.lbl_userid = _lbluserid
                if _lastfuel is not None:
                    self.lbl_lastfuel = _lastfuel
                if _lblvechs is not None:
                    self.lbl_vechs = _lblvechs
                if _lblEthIP is not None:
                    self.lbl_eth_ip = _lblEthIP
                if _lblWiFiIP is not None:
                    self.lbl_wifi_ip = _lblWiFiIP
            except Exception:
                pass

            # Nozzle → image: imggun (IN/OUT'a göre ikon)
            try:
                self.imggun = b.get_object("imggun")  # Gtk.Image
            except Exception:
                self.imggun = None

            # Station/Pump → image: imgpump (FILLING/COMPLETED'a göre ikon)
            try:
                self.imgpump = b.get_object("imgpump")  # Gtk.Image
            except Exception:
                self.imgpump = None
            # Vehicle/Heç image
            try:
                self.imgvhec = b.get_object("imgvhec")  # Gtk.Image
            except Exception:
                self.imgvhec = None
            # Buttons:
            try:
                _btnAuth = b.get_object("btnAuth")    # btn_start_auth
                if _btnAuth is not None:
                    self.btn_start_auth = _btnAuth
                    # Glade'de handler yoksa güvenli bağ:
                    if hasattr(self, "on_start_authorized"):
                        _btnAuth.connect("clicked", self.on_start_authorized)
            except Exception:
                pass

            # Combobox (port):
            try:
                _cmbCom = b.get_object("comboxCom")   # cmb_port
                if _cmbCom is not None:
                    self.cmb_port = _cmbCom
                    # Glade'den gelen combobox'ı hemen doldur
                    try:
                        self._refresh_ports()
                    except Exception:
                        pass
            except Exception:
                pass

            # Spin/Label: addr (GUI'de sadece gösterim için lblGprsIP kullanılıyor)
            try:
                _gprs_head = b.get_object("lblGprsIP")  # sadece görüntüleme
                if _gprs_head is not None:
                    try:
                        _gprs_head.set_text(f"{DEFAULT_ADDR:02X}")
                    except Exception:
                        pass
            except Exception:
                pass

            # Status toast (varsa guide olacak)
            try:
                _toast = b.get_object("status_toast")
                if _toast is not None:
                    self._status_toast = _toast
            except Exception:
                pass

            # 3) Yalnızca Glade’de olan widget’lar → mapping
            wanted_ids = [
                "MainWindow","boxhome","header_grid","gridtime","gridconn","grid-modver",
                "gridlevel","gridpump","gridgunpump","gridprocess","griddurumhead","gridmsg",
                "gridfooter","gridfootrecs","gridfootvech","gridfootcounter","gridfootIP","gridstrtbtn",
                "lbldate","lbltime","lblmodel","lblvers","lbllevlbl","lbllevel","lblpbarhead",
                "imgwifi","imggsm","imggps","imglan"
            ]
            self.ui = {}
            for wid in wanted_ids:
                try:
                    obj = b.get_object(wid)
                    if obj is not None:
                        self.ui[wid] = obj
                except Exception:
                    pass

            # 4) CSS-ID ↔ Glade-ID runtime ad atamaları (Glade dosyasını değiştirmeden)
            css_name_map = {
                "MainWindow": "MainWindow",
                "boxhome": "home-box",
                "header_grid": "head-grid",
                "gridtime": "time-grid",
                "gridconn": "conn-grid",
                "grid-modver": "modver-grid",
                "gridlevel": "process-grid",
                "griddurumhead": "durumhead-grid",
                "gridmsg": "msg-grid",
                "gridfooter": "footer-grid",
                "gridfootrecs": "footerrecs-grid",
                "gridfootvech": "footvech-grid",
                "gridfootcounter": "foodcount-grid",
                "lbldate": "date-label",
                "lbltime": "time-label",
                "lblmodel": "model-label",
                "lblvers": "vers-label",
                "lbllevlbl": "levlbl-label",
                "lbllevel": "level-label",
                "lblpbarhead": "pbarhead-label",
            }
            for gid, css_name in css_name_map.items():
                obj = self.ui.get(gid)
                if obj is not None:
                    try:
                        obj.set_name(css_name)
                    except Exception:
                        pass
            # 5) Glade'i KÖK UI olarak pencereye adopt et (MainWindow → child)
            try:
                glade_win = b.get_object("MainWindow")
                adopted = False
                if isinstance(glade_win, Gtk.Window):
                    child = glade_win.get_child()
                    if child is not None:
                        glade_win.remove(child)
                        self.add(child)
                        self._glade_root = child
                        adopted = True
                        sys.stderr.write("[GLADE-ADOPT] adopted child of MainWindow\n")
                # Fall-back: MainWindow çocuğu yoksa 'boxhome' konteynerini doğrudan ekle
                if not adopted:
                    boxhome = b.get_object("boxhome")
                    if boxhome is not None:
                        try:
                            par = boxhome.get_parent() if hasattr(boxhome, "get_parent") else None
                            if par:
                                par.remove(boxhome)
                        except Exception:
                            pass
                        self.add(boxhome)
                        self._glade_root = boxhome
                        adopted = True
                        sys.stderr.write("[GLADE-ADOPT] adopted 'boxhome' as root\n")
                if not adopted:
                    sys.stderr.write("[GLADE-ADOPT-WARN] No adopt target found (ne MainWindow child ne de boxhome)\n")
            except Exception as e:
                sys.stderr.write(f"[GLADE-ADOPT-WARN] {e}\n")
            # 6) (İsteğe bağlı) HOME arka plan rengini Python'dan sabit değere zorla
            try:
                hex_color = os.environ.get("HOME_BG_HEX", "#2A7593")  # burada istediğin rengi verebilirsin
                self._apply_homebox_bg(hex_color)
            except Exception:
                pass
        # Glade mapping tamamlandıktan sonra, ilk idle'da port listesini bir kez daha yenile
        try:
            GLib.idle_add(self._refresh_ports)
        except Exception:
            pass
        # 5) Icon theme arama yoluna /gui/resource/ ekle
        try:
            Gtk.IconTheme.get_default().append_search_path(str(RES_DIR))
        except Exception:
            pass
        # Logger: configs/logs.csv (dosya yoksa başlıkla oluşturur)
        try:
            cfgdir = BASE_DIR / "configs"
            self._logger = CsvLogger(cfgdir / "logs.csv") if CsvLogger else None
        except Exception:
            self._logger = None
        # Bağlantı ikonlarını hemen bir kez güncelle ve sonra 2 sn’de bir tekrar et
        try:
            self._update_conn_icons()
            GLib.timeout_add_seconds(2, self._update_conn_icons_timer)
        except Exception:
            pass
    def on_pump_status(self, state: str):
        """Pompadan PARSE edilen gerçek durum için çağrılır (DC1/DC3)."""
        self._apply_visual_state(state)
        # 1) lblmsg: duruma göre kullanıcı mesajı
        try:
            st = (state or "").upper()
            msg = None
            if st == "FILLING":
                msg = "DOLUM YAPILIYOR"
            elif st == "SUSPENDED":
                msg = "DOLUM DURAKLATILDI"
            elif st == "AUTHORIZED":
                msg = "POMPA HAZIR"
            elif st == "MAX AMOUNT/VOLUME":
                msg = "LİMİTE ULAŞILDI"
            elif st == "SWITCHED OFF":
                msg = "SİSTEM KAPALI"
            elif st == "NOT PROGRAMMED":
                msg = "PROGRAMLANMAMIŞ"
            elif st == "RESET":
                msg = "RESET"
            elif st == "FILLING COMPLETED":
                msg = "POMPA HAZIR"
            if msg:
                self._set_msg(msg)
        except Exception:
            pass
        # 1b) AUTH var/yok → lbluserid ve araç ikonu
        try:
            st_up = (state or "").upper()
            auth_active = st_up in ("AUTHORIZED","FILLING","SUSPENDED")
            if getattr(self, "lbl_userid", None):
                self.lbl_userid.set_text("Yetkili Kullanıcı" if auth_active else "--.---.--")
            self._set_vehicle_icon(on=auth_active)
        except Exception:
            pass
        # Nozzle/Tabanca ikonu: SUSPENDED ise özel ikon; değilse IN/OUT metnine göre
        try:
            st_up = (state or "").upper()
            if "SUSPENDED" in st_up:
                if getattr(self, "imggun", None) is not None:
                    p = RES_DIR / "gun_pump_suspend_64x64.png"
                    if p.exists():
                        self.imggun.set_from_file(str(p))
            else:
                self._update_nozzle_icon()
        except Exception:
            pass

        # State'e göre istasyon/pompa ikonunu da güncelle (FILLING/COMPLETED/SUSPENDED)
        try:
             self._update_station_icon(state)
        except Exception:
             pass
        # Eğer dolum bitti ise tabanca ikonunu 'off' yap + level reset/lastfuel güncelle
        try:
            st_up = (state or "").upper()
            if st_up in ("FILLING COMPLETED", "MAX AMOUNT/VOLUME"):
                if getattr(self, "imggun", None):
                    p = RES_DIR / "gun_pump_off_64x64.png"
                    if p.exists():
                        self.imggun.set_from_file(str(p))
                # lastfuel: son dolumu sakla (sıfırlanmaz)
                try:
                    # Önce sale tracker'dan al, yoksa lbllevel text'inden oku
                    last_l = None
                    if getattr(self, "_sale_last_vol_l", None) is not None:
                        last_l = float(self._sale_last_vol_l)
                    elif getattr(self, "_glade_level_label", None):
                        try:
                            last_l = float((self._glade_level_label.get_text() or "0").replace(",", "."))
                        except Exception:
                            last_l = 0.0
                    else:
                        last_l = 0.0
                    self._dbg(f"terminal state '{st_up}' last_l={last_l:.2f}")
                    # Ekrandaki "lastfuel" etiketi (sıfırlanmaz)
                    if getattr(self, "lbl_lastfuel", None) and last_l is not None:
                        self.lbl_lastfuel.set_text(f"{last_l:.1f}")
                except Exception:
                    pass
                # lbllevel: 0.0'a çek
                try:
                    if getattr(self, "_glade_level_label", None):
                        self._glade_level_label.set_text("0.0")
                except Exception:
                    pass
        except Exception:
            pass
            self._update_station_icon(state)
        except Exception:
            pass
        # Also drop a one-line note to Parsed pane if available:
        try:
            buf = self.tv_parsed.get_buffer()
            end = buf.get_end_iter()
            buf.insert(end, f"\n[STATE] -> {self._state}\n")
        except Exception:
            pass

    def on_nozzle_event(self, out_bool: bool):
        """
        Sim'den gelen 0xD4 frame'ine göre tabanca durumu güncelle:
        True  → NOZZLE OUT (gun lifted)
        False → NOZZLE IN  (gun hung up)
        """
        self._nozzle_out = bool(out_bool)
        # Label metnini gÃ¼ncelle
        if self._nozzle_out:
            # OUT = aktif, gun kaldÄ±rÄ±ldÄ±
            txt = "NOZZLE: OUT"
        else:
            txt = "NOZZLE: IN"
        try:
            self.lbl_nozzle.set_text(txt)
        except Exception:
            pass
        try:
            self.lbl_nozzle.set_text(txt)
        except Exception:
            pass
        # EV log: Nozzle değişimi (GunOn / GunOff)
        try:
            self._log(f"[EV] logCode={'GunOn' if out_bool else 'GunOff'}")
        except Exception:
            pass
        # Parsed paneline de k\u0131sa not d\u00fc\u015f
        try:
            buf = self.tv_parsed.get_buffer()
            end = buf.get_end_iter()
            buf.insert(end, f"\n[NOZZLE] -> {txt}\n")
        except Exception:
            pass
    # Optional convenience: call after sending local commands to show intent
    def _hint_state_intent(self, intent_state: str):
        # Yalnızca görsel ipucu ver; PARSE paneline satır düşme.
        # Pompa ile **henüz** el sıkışılmadıysa (HS yoksa) görseli değiştirme.
        if self._hs_ok:
            self._apply_visual_state(intent_state)

    # ---------- NEW: Help (F1) & Quick Status (F2) ----------
    # Zengin yardım: uzun metni dosyadan oku (UTF-8) ve kaydırılabilir dialogda göster
    def _load_help_text(self) -> str:
        base = os.path.dirname(os.path.abspath(__file__))
        for rel in (os.path.join("docs", "Help_Ca_TR.md"), "Help_Ca_TR.md"):
            p = os.path.join(base, rel)
            try:
                with open(p, "r", encoding="utf-8") as f:
                    return f.read()
            except Exception:
                continue
        return ("Help dosyası bulunamadı.\n"
                "Lütfen docs/Help_Ca_TR.md dosyasını ekleyin.")

    def _show_rich_help(self):
        dlg = Gtk.Dialog(title="Yardım — Controller / Protokol Özeti",
                         transient_for=self, flags=0)
        dlg.set_modal(True)
        dlg.set_default_size(860, 640)
        box = dlg.get_content_area()
        sw = Gtk.ScrolledWindow()
        sw.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        tv = Gtk.TextView()
        tv.set_editable(False)
        tv.set_wrap_mode(Gtk.WrapMode.WORD)
        buf = tv.get_buffer()
        buf.set_text(self._load_help_text())
        sw.add(tv)
        box.pack_start(sw, True, True, 0)
        btn = dlg.add_button("Kapat", Gtk.ResponseType.CLOSE)
        btn.grab_default()
        dlg.show_all()
        dlg.run()
        dlg.destroy()
    def _on_key_press(self, _widget, event):
        # Gdk keycode sabitleriyle doÄŸrudan karÅŸÄ±laÅŸtÄ±r (platform gÃ¼venli)
        kv = event.keyval
        # Gdk.KEY_F1/F2 mevcut; bazÄ± ortamlarda sayÄ±sal deÄŸerler iÃ§in fallback ekleyelim
        if kv in (getattr(Gdk, "KEY_F1", 65470), 65470):
            self._show_rich_help()
            return True
        if kv in (getattr(Gdk, "KEY_F2", 65471), 65471):
            self._toggle_status_toast()
            return True
        return False

    def _show_help_dialog(self):
        # 00:49:47 ve 01:52:58 mesajlarÄ±nÄ±n birleÅŸik Ã¶zeti
        text = (
            "PUMP STATE MACHINE (özet)\n"
            "RESET → AUTHORIZE → AUTHORIZED → (Nozzle OUT) → FILLING\n"
            "SUSPEND ⇄ RESUME (AUTHORIZED/FILLING)\n"
            "FILLING → STOP veya Nozzle IN → FILLING COMPLETED\n"
            "FILLING → Preset doldu → MAX AMOUNT/VOLUME\n"
            "Her yerden: SWITCH OFF → SWITCHED OFF\n\n"
            "Controller → Pump (yapabildiklerin)\n"
            "• RETURN STATUS / IDENTITY / FILLING INFO\n"
            "• RESET  – sayaç/ekran/preset sıfırlar\n"
            "• AUTHORIZE (Start) – doluma hazırlık, uygun ise motor ON\n"
            "• SUSPEND / RESUME  – geçici durdur / devam et\n"
            "• STOP / SWITCH OFF – dolumu bitir / ekipmanı kapat\n"
            "• Allowed Nozzle Numbers (CD2) – yetkili nozul listesi\n"
            "• Preset Volume/Amount (CD3/CD4) – hedef litre/tutar\n"
            "• Price Update (CD5) – fiyatları yükle (dolum başlamadan)\n"
            "• Volume Total Counters (CD101) – toplam sayaçlar\n\n"
            "Pump → Controller (olay/veri)\n"
            "• DC1: Durum (NOT PROGRAMMED, RESET, AUTHORIZED, FILLING,\n"
            "        SUSPENDED, FILLING COMPLETED, MAX AMOUNT/VOLUME, SWITCHED OFF)\n"
            "• DC2: Dolan Hacim/Tutar (packed/BCD değilse ml & cent örneği)\n"
            "• DC3: Nozzle in/out & uygulanan fiyat\n"
            "• DC101: Volume total counters (isteğe bağlı)\n\n"
            "Notlar\n"
            "• Start tipik olarak AUTHORIZE’dır. Fiyat/Nozzle/Preset opsiyoneldir\n"
            "  ancak dolum başlamadan programlanmalıdır.\n"
            "• Controller hesap yapmaz; pompanın bildirdiğini ekrana yazar.\n\n"
            "Kısayollar: F1=Yardım (bu pencere), F2=Durum balonu, ESC=Kapat"
        )
        dlg = Gtk.MessageDialog(
            transient_for=self, flags=0,
            message_type=Gtk.MessageType.INFO,
            buttons=Gtk.ButtonsType.CLOSE,
            text="Durum Makinesi Yardımı",
        )
        dlg.format_secondary_text(text)
        dlg.set_modal(True)
        # ESC ile kapat (bazÄ± platformlarda garanti olsun diye)
        def _esc_close(w, ev):
            kv = ev.keyval
            if kv in (getattr(Gdk, "KEY_Escape", 65307), 65307):
                w.response(Gtk.ResponseType.CLOSE)
                return True
            return False
        dlg.connect("key-press-event", _esc_close)
        dlg.run()
        dlg.destroy()
    def _toggle_status_toast(self):
        try:
            if self._status_toast is None:
                self._status_toast = Gtk.Window(
                    title="Pump Status", type=Gtk.WindowType.TOPLEVEL
                )
                self._status_toast.set_transient_for(self)
                self._status_toast.set_decorated(False)
                self._status_toast.set_keep_above(True)
                self._status_toast.set_resizable(False)
                box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
                box.set_border_width(8)
                self._toast_label = Gtk.Label(label=f"{self._state_led}  {self._state}")
                box.pack_start(self._toast_label, False, False, 0)
                self._status_toast.add(box)
                self._status_toast.show_all()
            else:
                if self._status_toast.get_visible():
                    self._status_toast.hide()
                else:
                    self._status_toast.show_all()
            # keep text fresh
            if hasattr(self, "_toast_label"):
                self._toast_label.set_text(f"{self._state_led}  {self._state}")
        except Exception:
            pass


    # --- SATIŞ penceresi & SALE_DIAG tetikleyici (ortak yardımcı) ---
    def _sale_update_on_state(self, canon: str):
        """
        Canonical state değişimlerine göre satış penceresini yönetir ve
        terminale geçişte (FILLING COMPLETED / MAX AMOUNT/VOLUME) SALE_DIAG satırını üretir.
        """
        try:
            prev_state = getattr(self, "_sale_state", None)
            # 0) AUTH başarısızlığı: AUTH pending iken NOT PROGRAMMED/RESET'e döndüyse NoAuth say
            if (canon in ("NOT PROGRAMMED", "RESET")) and \
               (getattr(self, "_auth_pending_after_preset", False) or getattr(self, "_auth_pending_for_nozzle", False)) and \
               (prev_state not in ("AUTHORIZED", "FILLING")):
                try:
                    self._log_csv_event("NoAuth")
                except Exception:
                    pass
                # pending bayraklarını temizle
                try:
                    self._auth_pending_after_preset = False
                    self._auth_pending_for_nozzle = False
                except Exception:
                    pass
            # 1) Satış penceresine giriş: AUTHORIZED/FILLING'e ilk kez girildi
            if (canon in ("AUTHORIZED", "FILLING")) and (prev_state not in ("AUTHORIZED", "FILLING")):
                self._sale_active = True
                self._sale_has_dc2 = False
                self._sale_last_vol_raw = None
                self._sale_last_amo_raw = None
                self._sale_last_vol_l = None
                self._sale_last_amo_unit = None
                self._last_nozzle_logged = None
                # AUTH başarılı (satışa ilk giriş)
                try:
                    self._log_csv_event("AuthOk")
                except Exception:
                    pass
                # Yeni satış başlarken ekranda anlık litreyi sıfırla
                try:
                    if getattr(self, "_glade_level_label", None):
                        self._glade_level_label.set_text("0.0")
                except Exception:
                    pass
            # 2) Terminale giriş: SATIŞ BİTTİ ANI
            if (canon in ("FILLING COMPLETED", "MAX AMOUNT/VOLUME")) and (prev_state not in ("FILLING COMPLETED", "MAX AMOUNT/VOLUME")):
                if getattr(self, "_sale_active", False):
                    if getattr(self, "_sale_has_dc2", False):
                        try:
                            vol_l = self._sale_last_vol_l
                            amo_unit = self._sale_last_amo_unit
                            self.append_tv(self.tv_parsed, f"[SALE_DIAG] SALE COMPLETE with DC2: VOL={vol_l:.2f} L AMO={amo_unit:.2f} (state={canon})")
                        except Exception:
                            self.append_tv(self.tv_parsed, f"[SALE_DIAG] SALE COMPLETE with DC2 (state={canon})")
                    if getattr(self, "_sale_has_dc2", False):
                        try:
                            vol_l = self._sale_last_vol_l
                            amo_unit = self._sale_last_amo_unit
                            self.append_tv(self.tv_parsed, f"[SALE_DIAG] SALE COMPLETE with DC2: VOL={vol_l:.2f} L AMO={amo_unit:.2f} (state={canon})")
                        except Exception:
                            self.append_tv(self.tv_parsed, f"[SALE_DIAG] SALE COMPLETE with DC2 (state={canon})")
                    else:
                        self.append_tv(self.tv_parsed, f"[SALE_DIAG] SALE COMPLETE but no DC2 seen in this sale (state={canon})")
                    # --- Sayaçlar: vechs+1, total_l += last_l (tek sefer) ---
                    try:
                        last_l = None
                        if getattr(self, "_sale_last_vol_l", None) is not None:
                            last_l = float(self._sale_last_vol_l)
                        elif getattr(self, "_glade_level_label", None):
                            # DC2 hiç gelmediyse lbllevel üzerindeki değeri yedek olarak kullan
                            try:
                                last_l = float((self._glade_level_label.get_text() or "0").replace(",", "."))
                            except Exception:
                                last_l = 0.0
                        else:
                            last_l = 0.0
                        # Kalıcı sayaç güncelle (data/counters.json) ve lblvechs/lblcounter'ı güncelle
                        self._bump_counters_once(last_l)
                    except Exception:
                        pass

            # 3) Terminal/reset durumlarında pencereyi kapat (& bazılarını tam temizle)
                if canon in ("FILLING COMPLETED","MAX AMOUNT/VOLUME","RESET","SWITCHED OFF","NOT PROGRAMMED"):
                    self._sale_active = False
                    # CD3 tek-sefer guard bayrağını kapat
                    self._cd3_sent_in_this_sale = False
                # CD3 tek-sefer guard bayrağını kapat
                self._cd3_sent_in_this_sale = False
                if canon in ("RESET", "SWITCHED OFF", "NOT PROGRAMMED"):
                    self._sale_has_dc2 = False
                    self._sale_last_vol_raw = None
                    self._sale_last_amo_raw = None
                    self._sale_last_vol_l = None
                    self._sale_last_amo_unit = None

                # --- CSV satış logu (minimal): fuel = bu satıştaki litre
                try:
                    if getattr(self, "_logger", None) and getattr(self, "_sale_last_vol_l", None) is not None:
                        fuel_l = f"{float(self._sale_last_vol_l):.2f}"
                        self._logger.append(
                            rfid       = getattr(self, "last_rfid", ""),
                            firstName  = getattr(self, "last_user_first", ""),
                            lastName   = getattr(self, "last_user_last", ""),
                            plate      = getattr(self, "last_user_plate", ""),
                            limit_val  = getattr(self, "last_user_limit", ""),
                            fuel       = fuel_l,
                            logCode    = "FillOk",
                            sendOk     = "NA"
                        )
                except Exception:
                    pass
            # Son görülen canonical state'i kaydet
            self._sale_state = canon
        except Exception:
            pass
    # --- LED Ã§izimi ---
    def _on_led_draw(self, da: Gtk.DrawingArea, cr):
        w = da.get_allocated_width()
        h = da.get_allocated_height()
        r = min(w, h) / 2 - 1
        cx, cy = w / 2.0, h / 2.0
        # fill
        cr.set_source_rgb(*self._led_color)
        cr.arc(cx, cy, r, 0, 2 * math.pi)
        cr.fill()
        # border
        cr.set_source_rgb(0, 0, 0)
        cr.arc(cx, cy, r, 0, 2 * math.pi)
        cr.stroke()
        return False
    # ---------- (OPTIONAL) call hints on local TX ----------
    def cmd_authorize(self, *args, **kwargs):
        # existing TX logic...
        # send AUTHORIZE ...
        self._hint_state_intent("AUTHORIZED")
        # return existing result

    def cmd_suspend(self, *args, **kwargs):
        # send SUSPEND ...
        self._hint_state_intent("SUSPENDED")

    def cmd_resume(self, *args, **kwargs):
        # send RESUME ...
        self._hint_state_intent("AUTHORIZED")  # or FILLING, depending on nozzle

    def cmd_stop(self, *args, **kwargs):
        # send STOP ...
        self._hint_state_intent("FILLING COMPLETED")

    def cmd_reset(self, *args, **kwargs):
        # send RESET ...
        self._hint_state_intent("RESET")

    def cmd_switch_off(self, *args, **kwargs):
        # send SWITCH OFF ...
        self._hint_state_intent("SWITCHED OFF")
    def _bold(self, lbl: Gtk.Label):
        al = Pango.AttrList(); al.insert(Pango.attr_weight_new(Pango.Weight.BOLD))
        lbl.set_attributes(al)

    def _mk_textview(self) -> Gtk.TextView:
        tv = Gtk.TextView()
        tv.set_editable(False); tv.set_monospace(True)
        return tv

    # --- Tek seferlik tag oluşturucu (varsa yeniden kullanır) ---
    def _ensure_tag(self, tv, name, **props):
        buf = tv.get_buffer()
        table = buf.get_tag_table()
        tag = table.lookup(name)
        if tag is None:
            tag = buf.create_tag(name, **props)
        return tag

    def _build_labeled_view(self, title: str, tv: Gtk.TextView) -> Gtk.Box:
        """
        TextView'i bir başlık label (ör: 'TX', 'RX', 'PARSE') ve scroll ile
        dikey kutuya koyar. Paned içine tek widget olarak gömeriz.
        """
        wrap = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        lbl = Gtk.Label(label=title)
        lbl.set_xalign(0.0)
        self._bold(lbl)
        wrap.pack_start(lbl, False, False, 0)

        sw = Gtk.ScrolledWindow()
        sw.add(tv)
        wrap.pack_start(sw, True, True, 0)

        return wrap
    # -------- NEW: TextView helpers (TR labels) --------
    def _tv_select_all(self, tv: Gtk.TextView):
        buf = tv.get_buffer()
        start, end = buf.get_start_iter(), buf.get_end_iter()
        buf.select_range(start, end)

    def _tv_copy_selection(self, tv: Gtk.TextView):
        buf = tv.get_buffer()
        if not buf.get_has_selection():
            return
        start, end = buf.get_selection_bounds()
        text = buf.get_text(start, end, True)
        cb = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        cb.set_text(text, -1)

    def _tv_delete_selection(self, tv: Gtk.TextView):
        buf = tv.get_buffer()
        if buf.get_has_selection():
            start, end = buf.get_selection_bounds()
            buf.delete(start, end)

    def _tv_clear_all(self, tv: Gtk.TextView):
        buf = tv.get_buffer()
        buf.delete(buf.get_start_iter(), buf.get_end_iter())

    def _attach_tv_context(self, tv: Gtk.TextView, allow_clear: bool = True):
        menu = Gtk.Menu()
        mi_selall = Gtk.MenuItem.new_with_label("Tümünü Seç")
        mi_copy   = Gtk.MenuItem.new_with_label("Kopyala")
        mi_delete = Gtk.MenuItem.new_with_label("Seçimi Sil")
        mi_sep1   = Gtk.SeparatorMenuItem()
        mi_clear  = Gtk.MenuItem.new_with_label("Temizle")

        mi_selall.connect("activate", lambda *_: self._tv_select_all(tv))
        mi_copy.connect("activate",   lambda *_: self._tv_copy_selection(tv))
        mi_delete.connect("activate", lambda *_: self._tv_delete_selection(tv))
        mi_clear.connect("activate",  lambda *_: self._tv_clear_all(tv))

        for it in (mi_selall, mi_copy, mi_delete, mi_sep1):
            menu.append(it)
        if allow_clear:
            menu.append(mi_clear)
        menu.show_all()

        def _on_button_press(widget, event):
            try:
                if event.type == Gdk.EventType.BUTTON_PRESS and event.button == 3:
                    buf = tv.get_buffer()
                    has_sel = bool(buf.get_has_selection())
                    mi_copy.set_sensitive(has_sel)
                    mi_delete.set_sensitive(has_sel)
                    menu.popup_at_pointer(event)
                    return True
            except Exception:
                pass
            return False
        tv.connect("button-press-event", _on_button_press)

    def _refresh_ports(self):
        # Uygulama kapanırken veya combobox çökmüşse hiç dokunma
        try:
            if getattr(self, "_shutting_down", False):
                return
            if not getattr(self, "cmb_port", None):
                return
            # Widget hâlâ hayatta mı? Toplevel varsa ve destroyed değilse devam et
            if hasattr(self.cmb_port, "get_toplevel"):
                top = self.cmb_port.get_toplevel()
                if top is None or (hasattr(top, "is_visible") and (not top.is_visible())):
                    return
        except Exception:
            return
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if not ports:
            ports = [DEFAULT_PORT]
        try:
            # Eğer ComboBoxText ise:
            if isinstance(self.cmb_port, Gtk.ComboBoxText):
                self.cmb_port.remove_all()
                for p in ports:
                    self.cmb_port.append_text(p)
            else:
                # Düz Gtk.ComboBox ise: basit bir ListStore + CellRendererText kur
                store = Gtk.ListStore(str)
                for p in ports:
                    store.append([p])
                self.cmb_port.set_model(store)
                # Hücre render'ı yoksa ekle
                if not self.cmb_port.get_cells():
                    cell = Gtk.CellRendererText()
                    self.cmb_port.pack_start(cell, True)
                    self.cmb_port.add_attribute(cell, "text", 0)
        except Exception:
            pass
        # DEFAULT_PORT varsa onu, yoksa ilkini seç
        try:
            if DEFAULT_PORT in ports:
                active = ports.index(DEFAULT_PORT)
            else:
                active = 0
            self.cmb_port.set_active(active)
        except Exception:
            pass

    def append_tv(self, tv, line):
        """
        TextView'a satır ekler. Eğer satır 'CRC_OK=False' içeriyorsa kırmızı ve kalın vurgular.
        """
        buf = tv.get_buffer()
        # Hedef aralık başlangıcını al (eklemeden önce)
        start_it = buf.get_end_iter()
        # Normal ekleme
        buf.insert(start_it, line + "\n")
        # Ekleme sonrası bitiş aralığı
        end_it = buf.get_end_iter()
        # Kötü CRC'leri kırmızı vurgula
        if "CRC_OK=False" in line:
            # bir defalık tag hazırla
            self._ensure_tag(tv, "badcrc", foreground="red", weight=Pango.Weight.BOLD)
            buf.apply_tag_by_name("badcrc", start_it, end_it)
        # İsteğe bağlı: log'a da geç
        self._log(line)
    def on_auto_poll_toggled(self, btn):
        """GUI'deki Auto POLL kutusu değiştiğinde heartbeat'i yönet."""
        # Port kapalıysa sadece timer'ı durdur; GUI durumu kalsın
        if not (self.ser and self.ser.is_open):
            self._hb_stop()
            return

        if btn.get_active():
            self.append_tv(self.tv_tx, f"[HB] Auto POLL ON ({self._hb_interval_ms} ms)")
            self._hb_wait_logged = False
            self._hb_start()
        else:
            self.append_tv(self.tv_tx, "[HB] Auto POLL OFF")
            self._hb_stop()
    def on_open_clicked(self, *_):
        if self.ser and self.ser.is_open:
            # close
            try:
                if self.reader: self.reader.stop()
            except: pass
            try:
                self.ser.close()
            except: pass
            self.ser = None; self.reader = None
            self.btn_conn.set_label("Open")
            self.append_tv(self.tv_tx, "[SER] CLOSED")
            self._hb_stop()
            self._hb_wait_logged = False
            self._hs_ok = False
            # Port kapanırken komutları kilitle
            self._set_controls_enabled(False)
            return

        port = self.cmb_port.get_active_text() or DEFAULT_PORT
        baud = int(self.spn_baud.get_value())
        # Parity seçimi
        par_map = {"None": serial.PARITY_NONE, "Even": serial.PARITY_EVEN, "Odd": serial.PARITY_ODD}
        par = par_map.get(self.cmb_par.get_active_text() or "Odd", serial.PARITY_ODD)
        # Stopbits seçimi
        stop = serial.STOPBITS_TWO if (self.cmb_stop.get_active_text() == "2") else serial.STOPBITS_ONE
        # Timeout (saniye)
        tout_s = max(0.005, float(self.spn_tout.get_value())/1000.0)
        try:
            self.ser = serial.Serial(
                port=port, baudrate=baud,
                bytesize=serial.EIGHTBITS, parity=par, stopbits=stop,
                timeout=tout_s, write_timeout=0.5
            )
            # CH340 RS485 Ã§oÄŸu durumda auto-DE yapar; ekstra RTS yÃ¶netimi gerekmez.
            self.reader = SerialReader(self.ser, self.rxq, self._on_serial_err, self._on_serial_raw)
            self.reader.start()
            # RX kanalı canlı mı?
            self.append_tv(self.tv_tx, f"[SER] READER alive={self.reader.is_alive()}")
            self.btn_conn.set_label("Close")
            self.append_tv(self.tv_tx, f"[SER] OPEN {port} @ {baud}")
            # Açılışta Auto POLL kapalı; HB kullanıcı isteğiyle başlayacak.
            self._hb_stop()
            self._hb_wait_logged = False
            # İlk açılışta Auto POLL checkbox'ını kapalıya çek (kullanıcı isterse açar)
            if hasattr(self, "chk_auto_poll"):
                self.chk_auto_poll.set_active(False)
            self.append_tv(
                self.tv_tx,
                "[HS] Otomatik el sıkışma kapalı. Gerekirse 'Auto POLL' ile 500 ms periyotlu POLL başlatın."
            )
            # Yeni port açıldı → HS bekleniyor; komutlar kilitli kalsın
            self._set_controls_enabled(False)

        except Exception as e:
            self.append_tv(self.tv_tx, f"[SER-ERR] {e}")
            # Port aÃ§Ä±lamadÄ± â†’ popup ile kullanÄ±cÄ±yÄ± uyar
            self._show_serial_error_dialog(str(e))
    # --- Boot sırasında otomatik bağlantı & Auto POLL başlatma ---
    def _startup_auto_open(self):
        """
        Uygulama açılışında:
          1) Varsayılan değerlerle seri portu aç
          2) Auto POLL'i etkinleştir ve heartbeat'i başlat
          3) 'Open' butonu zaten gizlenmiş durumda
        """
        # Eğer Glade combobox henüz aktif seçim yapmadıysa ilk porta al
        try:
            if getattr(self, "cmb_port", None) and self.cmb_port.get_active() < 0:
                self.cmb_port.set_active(0)
        except Exception:
            pass
        try:
            # Port kapalıysa aç
            if not (self.ser and self.ser.is_open):
                self.on_open_clicked()
        except Exception:
            pass
        try:
            # Auto POLL’i aç ve hemen HB’yi başlat
            if getattr(self, "chk_auto_poll", None):
                self.chk_auto_poll.set_active(True)
            self._hb_wait_logged = False
            self._hb_start()
            self.append_tv(self.tv_tx, "[BOOT] Auto-open + Auto-POLL enabled")
        except Exception:
            pass
        return False  # idle_add -> tek sefer çalışsın
    def on_send_dcc(self, _btn, dcc_val: int):
        # 1) Seri port açık mı?
        if not (self.ser and self.ser.is_open):
            self.append_tv(self.tv_tx, "[ERR] Serial not open"); return
        # 2) Handshake başarılandı mı? (pompa gerçekten var mı?)
        # Bilgi amaçlı istekler (RETURN_STATUS=0x00, RETURN_FILL_INFO=0x04) HS olmadan da serbest.
        is_info = dcc_val in (0x00, 0x04)
        if (not getattr(self, "_hs_ok", False)) and (not is_info):
            # HS yokken sadece durumu değiştiren komutları engelle
            self.append_tv(
                self.tv_tx,
                "[SAFE] HS yokken AUTHORIZE/START vb. gönderilmez — önce Auto POLL ile pompanın cevap verdiğini görün."
            )
            # Görsel ipucu vermeyelim; _hint_state_intent zaten _hs_ok şartlı.
            return

        try:
            # Güvenlik: Nozzle IN iken RESUME/START (0x0C) göndermeyi engelle
            if (not self._nozzle_out) and dcc_val == 0x0C:
                self.append_tv(self.tv_tx, "[SAFE] RESUME blocked: nozzle=IN")
                self.append_tv(self.tv_parsed, "[AUTO] nozzle IN → waiting pump COMPLETE; RESUME suppressed")
                return
            # AUTH gönderiliyorsa, olası "AUTH → sonra nozzle" senaryosu için bayrağı kur
            if dcc_val == 0x06:  # AUTHORIZE
                try:
                    self._auth_pending_for_nozzle = True
                    self._last_auth_ts = time.monotonic()
                except Exception:
                    self._auth_pending_for_nozzle = True

            # Terminal komutlarda bayrakları kapat
            if dcc_val in (0x08, 0x0A):  # STOP, SWITCH_OFF
                self._auth_pending_for_nozzle = False
                try:
                    self._auth_pending_after_preset = False
                except Exception:
                    self._auth_pending_after_preset = False

            # Asıl CD1 gönderimi
            self._send_cd1(dcc_val)

            # PROTOKOL ETİKETİ: R07 CD1 istek logu (TX tarafı)
            try:
                tag = {
                    0x00: "R07-CD1-RETURN_STATUS-REQ",
                    0x04: "R07-CD1-RETURN_FILL_INFO-REQ",
                    0x06: "R07-CD1-AUTHORIZE-REQ",
                    0x08: "R07-CD1-STOP-REQ",
                    0x0A: "R07-CD1-SWITCH_OFF-REQ",
                    0x0B: "R07-CD1-PAUSE-REQ",
                    0x0C: "R07-CD1-RESUME-REQ",
                }.get(dcc_val)
                if tag:
                    addr = int(self.spn_addr.get_value()) & 0xFF
                    self.append_tv(
                        self.tv_parsed,
                        f"[{tag}] addr={addr} dcc=0x{dcc_val:02X}"
                    )
            except Exception:
                # Log başarısız olsa bile komut gönderimi yapılmış durumda
                pass

            # TX sonrası "niyet" state'ini göster (gerçek DC1 gelince güncellenir)
            hint = {
                0x06: "AUTHORIZED",        # AUTHORIZE
                0x0B: "SUSPENDED",         # PAUSE
                0x0C: "AUTHORIZED",        # RESUME/START (nozzle OUT ise kÄ±sa sÃ¼rede FILLING'e geÃ§er)
                0x08: "FILLING COMPLETED", # STOP
                0x0A: "SWITCHED OFF",      # SWITCH_OFF
                0x04: None,                # RETURN_FILL_INFO -> sadece bilgi isteÄŸi
                0x00: None,                # RETURN_STATUS -> sadece bilgi isteÄŸi
            }.get(dcc_val)
            if hint:
                self._hint_state_intent(hint)
        except Exception as e:
            self.append_tv(self.tv_tx, f"[TX-ERR] {e}")

    def on_request_total_counters(self, _btn):
        """
        Volume Total Counters (CD101) isteğini tetikleyen UI handler'ı.
        TRANS=0x65, COUN=nozzle-1 varsayımıyla istek gönderir.
        """
        if not (self.ser and self.ser.is_open):
            self.append_tv(self.tv_tx, "[ERR] Serial not open (CD101)")
            return
        try:
            self._send_cd101_total_counters()
        except Exception as e:
            self.append_tv(self.tv_tx, f"[TX-ERR] CD101 TOTAL: {e}")
    # --- CD1 sender (controller -> device) + heartbeat touch ---
    def _send_cd1(self, dcc_val: int):
        addr = int(self.spn_addr.get_value()) & 0xFF
        nozzle = 0x01  # TODO: UI'dan seçilebilir; şimdilik nozzle-1
        # CD1 frame formatı (dokümana göre):
        # [ADDR][0x30][NOZ][LNG][DCC] + CRC(LO,HI) + 0x03 + 0xFA
        frame_wo_crc = bytes([addr & 0xFF, 0x30, nozzle & 0xFF, 0x01, dcc_val & 0xFF])
        crc = crc16_ibm(frame_wo_crc)
        # CRC byte order
        if (self.cmb_crc.get_active_text() or "LO,HI") == "HI,LO":
            crc_bytes = bytes([(crc >> 8) & 0xFF, crc & 0xFF])
        else:
            crc_bytes = bytes([crc & 0xFF, (crc >> 8) & 0xFF])
        out = frame_wo_crc + crc_bytes + bytes([ETX, TRAIL])
        self.ser.reset_output_buffer()
        self.ser.write(out)
        self.ser.flush()
        self.append_tv(self.tv_tx, "[TX] " + hexline(out))
        self._hb_touch()

    def _send_cd101_total_counters(self):
        """
        CD101 – Volume Total Counters isteği gönderir.
        Mepsan satış logundaki W:513C65010261D203FA örneğine uygun format:
          [ADDR][0x3C][0x65][0x01][NOZ] + CRC(LO,HI / HI,LO) + 0x03 + 0xFA
        """
        addr = int(self.spn_addr.get_value()) & 0xFF
        nozzle = 0x01  # TODO: UI'dan seçilebilir; şimdilik nozzle-1

        # Frame body (CRC hariç)
        frame_wo_crc = bytes([addr & 0xFF, 0x3C, 0x65, 0x01, nozzle & 0xFF])
        crc = crc16_ibm(frame_wo_crc)

        # CRC byte order, UI seçimine göre
        if (self.cmb_crc.get_active_text() or "LO,HI") == "HI,LO":
            crc_bytes = bytes([(crc >> 8) & 0xFF, crc & 0xFF])
        else:
            crc_bytes = bytes([crc & 0xFF, (crc >> 8) & 0xFF])

        out = frame_wo_crc + crc_bytes + bytes([ETX, TRAIL])
        try:
            self.ser.reset_output_buffer()
        except Exception:
            pass
        try:
            self.ser.write(out)
            self.ser.flush()
            self.append_tv(
                self.tv_tx,
                "[TX] " + hexline(out) + "  # CD101 TOTAL COUNTERS"
            )
            # PROTOKOL ETİKETİ: R07 CD101 totalizer isteği (TX tarafı)
            try:
                self.append_tv(
                    self.tv_parsed,
                    f"[R07-CD101-TOTAL-REQ] addr={addr & 0xFF} nozzle={nozzle & 0xFF}"
                )
            except Exception:
                pass
            self._hb_touch()
        except Exception as e:
            self.append_tv(self.tv_tx, f"[TX-ERR] CD101 TOTAL: {e}")
    def _send_cd3_preset_volume(self, liters: float):
        """
        CD3 – Preset Volume çerçevesi gönderir.
        Protokol örneği:
          5X 30 03 04 00 00 08 00 CRCLO CRCHI 03 FA
        Pump accepts this value as 8,00 lt preset
        Note: Decimal fraction is always zero.
        """
    # --- GUARD: Satış penceresi içinde CD3'ü sadece 1 kez gönder ---
        if getattr(self, "_sale_active", False) and getattr(self, "_cd3_sent_in_this_sale", False):
            try:
                self.append_tv(self.tv_tx, "[TX-SKIP] CD3 already sent in this sale")
            except Exception:
                pass
            return
        if not (self.ser and self.ser.is_open):
            self.append_tv(self.tv_tx, "[ERR] Serial not open (CD3)")
            return

        addr = int(self.spn_addr.get_value()) & 0xFF

        # Güvenli aralık: 0.1 .. 250.0 L (fazlasını kırpıyoruz)
        if liters < 0.1:
            liters = 0.1
        if liters > 250.0:
            liters = 250.0

        # Protokol: 8,00 L → 00000800 (x100 ölçek, BCD)
        # Yani VOL_BCD, litre*100 değerini taşır.
        raw = int(round(liters * 100.0))
        vol_bcd = _int_to_bcd4(raw)

        # [ADDR][0x30][TRANS=0x03][LNG=0x04][VOL(4)] + CRC + ETX + TRAIL
        frame_wo_crc = bytes([addr & 0xFF, 0x30, 0x03, 0x04]) + vol_bcd
        crc = crc16_ibm(frame_wo_crc)
        if (self.cmb_crc.get_active_text() or "LO,HI") == "HI,LO":
            crc_bytes = bytes([(crc >> 8) & 0xFF, crc & 0xFF])
        else:
            crc_bytes = bytes([crc & 0xFF, (crc >> 8) & 0xFF])

        out = frame_wo_crc + crc_bytes + bytes([ETX, TRAIL])
        try:
            self.ser.reset_output_buffer()
        except Exception:
            pass
        try:
            self.ser.write(out)
            self.ser.flush()
            self.append_tv(
                self.tv_tx,
                "[TX] " + hexline(out) + f"  # CD3 PRESET VOLUME (liters={liters:.2f})"
            )
            # Satış başına tek sefer gönderim: bayrağı işaretle
            self._cd3_sent_in_this_sale = True
            # PROTOKOL ETİKETİ: R07 CD3 PRESET isteği (TX tarafı)
            try:
                self.append_tv(
                    self.tv_parsed,
                    f"[R07-CD3-PRESET-REQ] addr={addr & 0xFF} liters={liters:.2f}"
                )
            except Exception:
                pass
        except Exception as e:
            self.append_tv(self.tv_tx, f"[TX-ERR] CD3 PRESET: {e}")

   # ---- Kısa (min) çerçeveler: POLL / ACK ----
    def _send_min_poll(self):
        if not (self.ser and self.ser.is_open):
            self.append_tv(self.tv_tx, "[ERR] Serial not open"); return
        out = bytes([0x50, 0x20, TRAIL])
        try:
            self.ser.reset_output_buffer()
            self.ser.write(out); self.ser.flush()
            self.append_tv(self.tv_tx, "[TX] " + hexline(out) + "  # MIN-POLL")
            self._hb_touch()
        except Exception as e:
            self.append_tv(self.tv_tx, f"[TX-ERR] MIN-POLL: {e}")

    def _send_min_ack(self):
        if not (self.ser and self.ser.is_open):
            return
        out = bytes([0x50, 0xC0, TRAIL])
        try:
            self.ser.write(out); self.ser.flush()
            self.append_tv(self.tv_tx, "[TX] " + hexline(out) + "  # MIN-ACK")
        except Exception as e:
            self.append_tv(self.tv_tx, f"[TX-ERR] MIN-ACK: {e}")
    def _on_serial_err(self, msg=None):
        """
        SerialReader thread'i bir hata bildirdiğinde çağrılır.
        msg bazı durumlarda None olabilir, bu yüzden güvenli string oluşturuyoruz.
        Bu fonksiyon thread-safe değil; GUI güncellemelerini GLib.idle_add ile ana threade atıyoruz.
        """
        if msg is None:
            norm_msg = "Seri haberleÅŸme hatasÄ± (detay yok)"
        else:
            norm_msg = str(msg)

        def _emit():
            # Eğer port artık KAPALIYSA (kapatma sÃ¼recinde geciken hata):
            #  - Pop-up GÃ–STERME
            #  - Sadece debug satÄ±rÄ±na yaz ve Ã§Ä±k
            # Eğer port artık KAPALIYSA (kapatma sürecinde geciken hata):
            # - Pop-up YOK
            # - Debug satırını sadece İLK KEZ yaz (spam koruması)
            if not (self.ser and self.ser.is_open):
                try:
                    if not hasattr(self, "_post_close_err_logged"):
                        self._post_close_err_logged = False
                    if not self._post_close_err_logged:
                        self.append_tv(self.tv_rx, f"[SER-DBG] (post-close) {norm_msg}")
                        self._post_close_err_logged = True
                except Exception:
                    pass
                return
            # Port aÃ§Ä±ksa, gerÃ§ek iletiÅŸim hatasÄ± kabul edip uyarÄ± gÃ¶ster
            self.append_tv(self.tv_rx, f"[SER-ERR] {norm_msg}")
            # İkonu 'ERR' yap
            try:
                if getattr(self, "imgpump", None):
                    p = RES_DIR / "station_err_64x64.png"
                    if p.exists():
                        self.imgpump.set_from_file(str(p))
            except Exception:
                pass
            self._show_serial_error_dialog(norm_msg)

        GLib.idle_add(_emit)

    def _poll_rx(self):
        try:
            while True:
                frame = self.rxq.get_nowait()
                self.append_tv(self.tv_rx, "[RX] " + hexline(frame))
                self._parse_and_update(frame)
                self._hb_touch()
        except queue.Empty:
            pass
        return True

    def _parse_and_update(self, fr: bytes):
        # Kısa (min) çerçeve: 0x50 0x20/0xC0/0x70 0xFA
        # Mepsan satış logunda:
        #   W:5020fa → R:5070fa
        # yani controller, 50 70 FA cevabına karşı ek bir 50 C0 FA göndermiyor. :contentReference[oaicite:6]{index=6}
        if len(fr) == 3 and fr[0] == 0x50 and fr[-1] == TRAIL:
            # 0x20: POLL, 0xC0: ACK, 0x70: sahadan gözlenen kısa cevap (BUSY/keepalive)
            code = fr[1]
            kind_map = {0x20: "MIN-POLL", 0xC0: "MIN-ACK", 0x70: "MIN-BUSY"}
            kind = kind_map.get(code, f"MIN-UNK(0x{code:02X})")
            self.append_tv(self.tv_parsed, f"[R07-MIN] {kind}")

            # AUTHORIZE zamanlaması: CD3 sonrası ilk MIN-BUSY'de tek seferlik gönder
            if kind == "MIN-BUSY" and getattr(self, "_auth_pending_after_preset", False):
                try:
                    self._send_cd1(0x06)  # AUTHORIZE
                    self._auth_pending_after_preset = False
                    self.append_tv(
                        self.tv_tx,
                        "[AUTH] AUTHORIZE (0x06) gönderildi [MIN-BUSY sonrası]"
                    )
                    self._hint_state_intent("AUTHORIZED")
                except Exception as e:
                    self.append_tv(self.tv_tx, f"[TX-ERR] AUTHORIZE@MIN: {e}")
            # Link canlı → SADECE pasif durumlarda 'POMPA HAZIR' yaz
            try:
                if (getattr(self, "_state", "") not in ("AUTHORIZED","FILLING","SUSPENDED","MAX AMOUNT/VOLUME")):
                    self._set_msg("POMPA HAZIR")
            except Exception:
                pass
            # Mepsan pompası ile bire bir uyum için:
            #  - 50 20 FA'ya gelen 50 70 FA cevabına ek MIN-ACK gönderME.
            #  - MIN çerçeveleri sadece loglanır ve heartbeat için "link canlı" sayılır.
            return
        # Uzun çerçeve: [ADDR][CMD][NOZ?][LEN?][PAYLOAD...][CRC][ETX][TRAIL]
        # Gerçek cihazda LEN sahası simdekiyle birebir örtüşmeyebilir;
        # bu yüzden header LEN'e sadece uyarı amaçlı bakıp, esas payload uzunluğunu
        # frame boyundan hesaplıyoruz.
        if len(fr) < 8:
            self.append_tv(self.tv_parsed, f"[PARSE] frame çok kısa len={len(fr)}")
            return

        addr, cmd, nozzle = fr[0], fr[1], fr[2]
        ln_hdr = fr[3]

        # Son 4 bayt: CRC_LO/CRC_HI, ETX, TRAIL
        # ÖNEMLİ: DC ailesi (0x31–0x3F) ve 0x65 (total counters) sahada
        # [ADDR][CMD][TRANS][LNG][DATA...] formatında.
        # Bu yüzden bu ailede payload TRANS'tan başlamalı (fr[2:-4]).
        if 0x31 <= cmd <= 0x3F or cmd == 0x65:
            payload = fr[2:-4]   # TRANS + LNG + DATA ...
        else:
            payload = fr[4:-4]   # Sim/diğer yollar: NOZ + LEN + DATA ...
        ln_actual = len(payload)
        # Not: 0x30–0x3F aralığındaki DC1/statik/DC-FILL/DC2/event ve 0x3E
        # (FILL-REC) çerçevelerinde sahada header LEN ile gerçek payload
        # uzunluğu bire bir örtüşmeyebiliyor. CRC_OK=True ise bunları normal
        # kabul ediyoruz; sadece DC ailesi dışındaki komutlarda LEN
        # uyuşmazlığını uyarı olarak yazıyoruz.
        suppress_len_warn = (0x30 <= cmd <= 0x3F)
        if (ln_hdr != ln_actual) and (not suppress_len_warn):
            self.append_tv(
                self.tv_parsed,
                f"[PARSE] bad LEN header={ln_hdr} actual={ln_actual}"
            )
        ln = ln_actual  # Bundan sonra gerçek payload uzunluğunu kullan

        # CRC sırası (LO,HI | HI,LO)
        if (self.cmb_crc.get_active_text() or "LO,HI") == "HI,LO":
            crc_hi, crc_lo = fr[-4], fr[-3]
        else:
            crc_lo, crc_hi = fr[-4], fr[-3]
        crc_rx = ((crc_hi & 0xFF) << 8) | (crc_lo & 0xFF)
        # Hesap body = ADDR..PAYLOAD (CRC/ETX/FA hariç)
        calc = crc16_ibm(fr[:-4])
        ok = (crc_rx == calc)

        # Saha gözlemine göre gerçek cihaz cevapları:
        #   0x30 : CD1/RETURN_STATUS yanıtları  → DC1 (TRANS=0x01) şeklinde işlenecek
        #   0x3E : FILLING RECORD
        # Simülasyon/alternatif akış için mevcut 0xCD/0xD1/0xD2/0xD3 yolları da korunur.
        if cmd in (0xCD, 0x30):
            # CD1 yanıtları (ACK/NAK vb.)
            info = f"[R07-CD1-RESP] payload={payload.hex().upper()} CRC_OK={ok}"
            self.append_tv(self.tv_parsed, info)
            # --- CD1 doğrudan state parse & SALE_DIAG tetik ---
            if ok and cmd == 0x30 and len(payload) == 1:
                st = payload[0]
                status_map = {
                    0x00: "NOT PROGRAMMED",
                    0x01: "RESET",
                    0x02: "AUTHORIZED",
                    0x03: "NOZZLE OUT",
                    0x04: "FILLING",
                    0x05: "FILLING COMPLETED",
                    0x06: "MAX AMOUNT/VOLUME",
                }
                canon = status_map.get(st, f"UNKNOWN(0x{st:02X})")
                self.append_tv(self.tv_parsed, f"[R07-DC1-STATUS-DIR] cmd=0x30 status={canon}")
                try:
                    self.on_pump_status(canon)
                except Exception:
                    pass
            # HS: Geçerli bir CD1/0x30 yanıtı gördük → hazır say
            if not self._hs_ok:
                self._hs_ok = True
                self.append_tv(self.tv_parsed, "[HS] DC1 görüldü → POMPA HAZIR")
                self._set_msg("POMPA HAZIR")
                # HS başarıldı → komut butonlarını aç
                self._set_controls_enabled(True)
                # Auto POLL açıksa HB'yi devrede tut
                try:
                    if (
                        self.ser and self.ser.is_open and
                        getattr(self, "chk_auto_poll", None) and
                        self.chk_auto_poll.get_active() and
                        not self._hb_timer_id
                    ):
                        self._hb_start()
                except Exception:
                    pass

            # Geçerli cevap gördük → min-ACK ile canlı tut (CRC doğruysa)
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass
            try:
                self._hs_ever_ok = True
                self._hs_last_ok_ts = time.monotonic()
            except Exception:
                pass
            # 0x30 yanıtını kanonik state'e çevirdik; satış akışını merkezi yardımcıya devret.
            try:
                self._sale_update_on_state(canon)
            except Exception:
                pass
            return
        # Gerçek pompa DC1 (Pump Status, TRANS=0x01, 1 byte durum)
        elif cmd == 0x01 and ln == 1 and len(payload) == 1:
            st = payload[0]
            # Dokümandaki "Pump Status" değerleri:
            # 00h NOT PROGRAMMED
            # 01h RESET
            # 02h AUTHORIZED
            # 04h FILLING
            # 05h FILLING COMPLETED
            # 06h MAX AMOUNT/VOLUME REACHED
            # 07h SWITCHED OFF
            # 0Bh PAUSED
            status_map = {
                0x00: "NOT PROGRAMMED",
                0x01: "RESET",
                0x02: "AUTHORIZED",
                0x04: "FILLING",
                0x05: "FILLING COMPLETED",
                0x06: "MAX AMOUNT/VOLUME",
                0x07: "SWITCHED OFF",
                0x0B: "PAUSED",
            }
            name = status_map.get(st, f"0x{st:02X}")
            # DC1 label'ını güncelle
            self.lbl_dc1.set_text(f"DC1: {name}")
            # Canonical GUI state'e eşle
            canon = {
                "NOT PROGRAMMED":      "NOT PROGRAMMED",
                "RESET":               "RESET",
                "AUTHORIZED":          "AUTHORIZED",
                "FILLING":             "FILLING",
                "FILLING COMPLETED":   "FILLING COMPLETED",
                "MAX AMOUNT/VOLUME":   "MAX AMOUNT/VOLUME",
                "SWITCHED OFF":        "SWITCHED OFF",
                "PAUSED":              "SUSPENDED",
            }.get(name, "RESET")
            self.on_pump_status(canon)
            # Doğrudan DC1 (cmd==0x01) geldiğinde de SALE_DIAG tetiklemesi için:
            try:
                self._sale_update_on_state(canon)
            except Exception:
                pass
            # AUTH sonrası bekleme bayrağı açıksa ve pompa artık net bir state'te ise kapat
            if getattr(self, "_auth_pending_for_nozzle", False):
                if name in ("FILLING", "FILLING COMPLETED", "MAX AMOUNT/VOLUME", "SWITCHED OFF", "RESET"):
                    self._auth_pending_for_nozzle = False

            self.append_tv(self.tv_parsed, f"[R07-DC1-STATUS] status={name} CRC_OK={ok}")

            # Geçerli DC1 gördüysek auto-ACK ve HS tamamlama
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass
            if not self._hs_ok:
                self._hs_ok = True
                self.append_tv(self.tv_parsed, "[HS] DC1 status görüldü → POMPA HAZIR")
                self._set_controls_enabled(True)
                # Auto POLL açıksa HB'yi devrede tut
                try:
                    if (
                        self.ser and self.ser.is_open and
                        getattr(self, "chk_auto_poll", None) and
                        self.chk_auto_poll.get_active() and
                        not self._hb_timer_id
                    ):
                        self._hb_start()
                except Exception:
                    pass
            try:
                self._hs_ever_ok = True
                self._hs_last_ok_ts = time.monotonic()
            except Exception:
                pass

        elif cmd == 0xD1 and ln == 1 and len(payload) == 1:
            # Simülasyon DC1 state çerçevesi
            st = payload[0]
            raw_states = {0x00:"IDLE", 0x01:"AUTHORIZED", 0x02:"FILLING",
                          0x03:"PAUSED", 0x04:"COMPLETE"}
            name = raw_states.get(st, hex(st))
            self.lbl_dc1.set_text(f"DC1: {name}")
            canon = {
                "IDLE": "RESET",
                "AUTHORIZED": "AUTHORIZED",
                "FILLING": "FILLING",
                "PAUSED": "SUSPENDED",
                "COMPLETE": "FILLING COMPLETED",
            }.get(name, "RESET")
            self.on_pump_status(canon)
            # Sim DC1 geldiğinde de aynı tetik
            try:
                self._sale_update_on_state(canon)
            except Exception:
                pass
            self.append_tv(self.tv_parsed, f"[R07-DC1-STATUS] status={name} CRC_OK={ok}")
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass
            if not self._hs_ok:
                self._hs_ok = True
                self.append_tv(self.tv_parsed, "[HS] DC1 status görüldü → POMPA HAZIR")
                # HS başarıldı → komut butonlarını aç
                self._set_controls_enabled(True)
                # Auto POLL açıksa HB'yi devrede tut
                try:
                    if (
                        self.ser and self.ser.is_open and
                        getattr(self, "chk_auto_poll", None) and
                        self.chk_auto_poll.get_active() and
                        not self._hb_timer_id
                    ):
                        self._hb_start()
                except Exception:
                    pass
            try:
                self._hs_ever_ok = True
                self._hs_last_ok_ts = time.monotonic()
            except Exception:
                pass

        elif cmd == 0xD2 and ln == 8 and len(payload) == 8:
            ml_raw   = _bcd4_to_int(payload[0:4])
            price    = _bcd4_to_int(payload[4:8])
            ml = ml_raw // 100
            self.lbl_dc2.set_text(f"DC2: ml={ml} price_cents={price}")
            self.append_tv(
                self.tv_parsed,
                f"[SIM-DC2] amount ml={ml} (raw={ml_raw}) price={price} CRC_OK={ok}"
            )
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass
            try:
                if (self._preset_target_ml is not None) and (not self._preset_stop_sent):
                    if ml >= int(self._preset_target_ml):
                        self.append_tv(self.tv_parsed, "[AUTO] preset hedefe ulaşıldı → STOP")
                        try:
                            self._send_cd1(0x08)  # STOP
                            self._preset_stop_sent = True
                            self._hint_state_intent("FILLING COMPLETED")
                        except Exception as e:
                            self.append_tv(self.tv_tx, f"[TX-ERR] STOP@preset: {e}")
            except Exception:
                pass

        elif cmd == 0xD3:
            if ln == 8 and len(payload) == 8:
                ml = _bcd4_to_int(payload[0:4])
                price = _bcd4_to_int(payload[4:8])
                self.lbl_dc3.set_text(f"DC3: total ml={ml} price_cents={price}")
                self.append_tv(
                    self.tv_parsed,
                    f"[R07-DC3-TOTAL] totals ml={ml} price={price} CRC_OK={ok}"
                )
                if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                    try:
                        self._send_min_ack()
                    except Exception:
                        pass

        elif cmd == 0x3D:
            # TOTALIZER (TRANS/LNG/DATA içinde BCD x100 totaller)
            self.append_tv(
                self.tv_parsed,
                f"[R07-3D-TOTAL] payload={payload.hex().upper()} CRC_OK={ok}"
            )
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass
            # Alt-kayıtları çözüp (TRANS=0x01, LEN=0x08) TOT_VOL/TOT_AMO yaz
            try:
               self._update_dc_from_payload(cmd, payload, ok=ok)
            except Exception as e:
                self.append_tv(self.tv_parsed, f"[R07-3D-DECODE-ERR] {e}")

        elif cmd == 0x3E:
            # FILLING RECORD (saha: 50 3E 01 01 04 ... 03 FA)
            self.append_tv(
                self.tv_parsed,
                f"[R07-FILL-REC] payload={payload.hex().upper()} CRC_OK={ok}"
            )
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass
            # Alt-kayıtları çözüp (TRANS=0x02, LEN=0x08) VOL/AMO değerlerini yaz
            try:
                self._update_dc_from_payload(cmd, payload, ok=ok)
            except Exception as e:
                self.append_tv(self.tv_parsed, f"[R07-3E-DECODE-ERR] {e}")
            # Eğer henüz HS tamamlanmadıysa ve geçerli bir FILL-REC gördüysek,
            # bunu da "pompa canlı" kabul et.
            if ok and not self._hs_ok:
                self._hs_ok = True
                self.append_tv(self.tv_parsed, "[HS] FILL-REC görüldü → POMPA HAZIR")
                self._set_controls_enabled(True)
                try:
                    if (
                        self.ser and self.ser.is_open and
                        getattr(self, "chk_auto_poll", None) and
                        self.chk_auto_poll.get_active() and
                        not self._hb_timer_id
                    ):
                        self._hb_start()
                except Exception:
                    pass
                try:
                    self._hs_ever_ok = True
                    self._hs_last_ok_ts = time.monotonic()
                except Exception:
                    pass

        elif cmd == 0x3F:
            # Saha: 3D ile birlikte gelen diğer event/özet blokları (decode TODO)
            self.append_tv(
                self.tv_parsed,
                f"[R07-3F-EVENT] payload={payload.hex().upper()} CRC_OK={ok}"
            )
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass

        elif cmd == 0xD4 and ln == 1 and len(payload) == 1:
            nozzle_flag = (payload[0] != 0x00)
            self.on_nozzle_event(nozzle_flag)
            self.append_tv(
                self.tv_parsed,
                f"[R07-D4-NOZZLE] nozzle={'OUT' if nozzle_flag else 'IN'} CRC_OK={ok}"
            )
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass

        else:
            # Saha gözlemi: 0x31–0x33 statik total blokları, 0x34–0x38 ise
            # dolum sırasında gelen DCx ailesi. 0x65 ise total counter cevabı.
            if 0x31 <= cmd <= 0x33:
                label = "[R07-DC-STATIC]"
            elif 0x34 <= cmd <= 0x38:
                label = "[R07-DC-FILL]"
            elif cmd == 0x3E:
                label = "[R07-FILL-REC]"
            elif cmd == 0x65:
                label = "[R07-DC-TOTAL]"
            else:
                label = "[UNHANDLED]"
            self.append_tv(
                self.tv_parsed,
                f"{label} cmd=0x{cmd:02X} ln={ln} CRC_OK={ok}"
            )
            # Eğer 0x31–0x33 statik total bloklarından birini ilk kez ve CRC_OK=True olarak
            # görürsek, bunu da HS için geçerli say (pompa hazır).
            if 0x31 <= cmd <= 0x33 and ok and not self._hs_ok:
                self._hs_ok = True
                self.append_tv(self.tv_parsed, "[HS] DC-STATIC görüldü → POMPA HAZIR")
                self._set_controls_enabled(True)
                # Auto POLL açıksa HB'yi devrede tut
                try:
                    if (
                        self.ser and self.ser.is_open and
                        getattr(self, "chk_auto_poll", None) and
                        self.chk_auto_poll.get_active() and
                        not self._hb_timer_id
                    ):
                        self._hb_start()
                except Exception:
                    pass
                try:
                    self._hs_ever_ok = True
                    self._hs_last_ok_ts = time.monotonic()
                except Exception:
                    pass
            # AUTH → nozzle sırasını sağlamlaştırma (ESKİ OTOMATİK re-AUTH BLOĞU DEVRE DIŞI):
            # Mepsan protokol PDF + gerçek satış loguna göre kontrolör tarafında
            # kendiliğinden ikinci AUTHORIZE gönderilmez. Saha gözlemlerinde bu
            # otomatik tekrar dolum akışını karmaşıklaştırabildiği için, bu blok
            # şimdilik devre dışı bırakıldı. Bundan sonra ikinci AUTHORIZE ancak
            # kullanıcı butona bastığında gönderilecek.

            # Bu ailedeki tüm geçerli cevaplarda auto-ACK gönder.
            if ok and getattr(self, "chk_auto_ack", None) and self.chk_auto_ack.get_active():
                try:
                    self._send_min_ack()
                except Exception:
                    pass

            # 0x34–0x38 aralığı (özellikle 0x35) geldiğinde ve daha önce HS yoksa
            # bunu da handshake tamamlandı olarak say.
            if ok and not self._hs_ok and (0x34 <= cmd <= 0x38):
                self._hs_ok = True
                self.append_tv(self.tv_parsed, "[HS] DC-FILL görüldü → POMPA HAZIR")
                self._set_controls_enabled(True)
                try:
                    if (
                        self.ser and self.ser.is_open and
                        getattr(self, "chk_auto_poll", None) and
                        self.chk_auto_poll.get_active() and
                        not self._hb_timer_id
                    ):
                        self._hb_start()
                except Exception:
                    pass
                try:
                    self._hs_ever_ok = True
                    self._hs_last_ok_ts = time.monotonic()
                except Exception:
                    pass
            # DC-FILL / FILL-REC / DC2 / event (0x34–0x3F) ve 0x65 (total counters)
            # çerçevelerinden DC1/DC2/DC3/DC101 alanlarını çıkartıp GUI'yi güncelle
            if ok and ((0x34 <= cmd <= 0x3F) or cmd == 0x65):
                try:
                    # 'ok' bilgisini de iletelim; 0x3E logunda CRC_OK'yi doğru yazacağız
                    self._update_dc_from_payload(cmd, payload, ok=ok)
                except Exception as e:
                    self.append_tv(self.tv_parsed, f"[DC-TR-ERR] decode: {e}")

            # --- AUTH → sonra nozzle senaryosu için otomatik tekrar AUTHORIZE ---
            # Mepsan satış logunda kontrolörün kendiliğinden ikinci AUTHORIZE
            # göndermesi gözlenmediği için, bu otomatik re-AUTH mantığı da
            # devre dışı bırakıldı. Akışın deterministik olması için AUTHORIZE
            # sadece UI üzerinden, kullanıcının açık isteğiyle gönderilecek.

    # 'ok' isteğe bağlı; eski Python sürümleri için birlik tip anotasyonu yok
    def _update_dc_from_payload(self, cmd: int, payload: bytes, ok=None):
        """
        0x34–0x3F (DC-FILL / DC2 / event ailesi) ve 0x3E (FILL-REC)
        çerçevelerinin payload'ını TRANS/LNG/DATA üçlüleri halinde açar
        ve DC1/DC2/DC3 bilgilerini GUI'ye yansıtır.
        """
        i = 0
        n = len(payload)
        # Bu payload işlenirken 'sınır dışı' uyarısını en fazla 1 kez yaz
        out_of_bounds_logged = False
        while i + 2 <= n:
            trans = payload[i]
            lng = payload[i + 1]
            if lng < 0 or i + 2 + lng > n:
                # Hatalı uzunluk – erken çık (kayıt başına tek uyarı)
                if not out_of_bounds_logged:
                    self.append_tv(
                        self.tv_parsed,
                        f"[DC-TR] trans=0x{trans:02X} len={lng} payload sınırı dışı (n={n}, i={i})"
                    )
                    out_of_bounds_logged = True
                break
            data = payload[i + 2 : i + 2 + lng]
            # --- 0x3E: Filling Record (VOL/AMO BCD x100) ---
            if cmd == 0x3E:
                try:
                        # Basit model: TRANS=0x02, LNG=0x08 -> VOL(4) + AMO(4)
                        if lng >= 0x08:
                            vol_raw = data[0:4]
                            amo_raw = data[4:8]
                            def bcd_to_int(b: bytes) -> int:
                                v = 0
                                for by in b:
                                    v = v*100 + ((by>>4)&0xF)*10 + (by&0xF)
                                return v
                            vol_l = bcd_to_int(vol_raw)/100.0
                            amo_u = bcd_to_int(amo_raw)/100.0
                            # Log: payload'ı kesin sınırla (TRANS+LEN+DATA) ve CRC_OK'yi ana frame 'ok'undan yaz
                            pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                            crc_str = "True" if (ok is True) else ("False" if (ok is False) else "N/A")
                            self.append_tv(self.tv_parsed,
                                        f"[R07-FILL-RECORD] payload={pl.hex().upper()} CRC_OK={crc_str}  "
                                        f"VOL={vol_l:.2f} L AMO={amo_u:.2f}")
                            # İstersek SALE_DIAG sonrası küçük teyit:
                            try:
                                self._sale_last_vol_l = vol_l
                                self._sale_last_amo_unit = amo_u
                            except Exception:
                                pass
                        else:
                            pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                            crc_str = "True" if (ok is True) else ("False" if (ok is False) else "N/A")
                            self.append_tv(self.tv_parsed, f"[R07-FILL-RECORD] payload={pl.hex().upper()} CRC_OK={crc_str}")
                except Exception as e:
                    pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                    self.append_tv(self.tv_parsed, f"[R07-FILL-RECORD-ERR] {e} payload={pl.hex().upper()}")
                i += 2 + lng
                continue

            # --- 0x3D: Totalizer (TRANS=0x01, LEN=0x08 -> TOT_VOL(4) + TOT_AMO(4), BCD x100) ---
            if cmd == 0x3D:
                try:
                    # CRC kötü ise totalizer özetini GUI'ye yansıtma — tek satır uyarı ile atla
                    if ok is False:
                        pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                        self.append_tv(
                            self.tv_parsed,
                            f"[R07-TOTALIZER] CRC_OK=False — skipped payload={pl.hex().upper()}"
                        )
                        i += 2 + lng
                        continue
                    if lng >= 0x08:
                        tv_raw = data[0:4]
                        ta_raw = data[4:8]
                        def bcd_to_int(b: bytes) -> int:
                            v = 0
                            for by in b:
                                v = v*100 + ((by>>4)&0xF)*10 + (by&0xF)
                            return v
                        tot_vol_l = bcd_to_int(tv_raw)/100.0
                        tot_amo_u = bcd_to_int(ta_raw)/100.0
                        pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                        crc_str = "True" if (ok is True) else ("False" if (ok is False) else "N/A")
                        self.append_tv(self.tv_parsed,
                                       f"[R07-TOTALIZER] payload={pl.hex().upper()} CRC_OK={crc_str}  "
                                       f"TOT_VOL={tot_vol_l:.2f} L TOT_AMO={tot_amo_u:.2f}")
                    else:
                        pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                        crc_str = "True" if (ok is True) else ("False" if (ok is False) else "N/A")
                        self.append_tv(self.tv_parsed, f"[R07-TOTALIZER] payload={pl.hex().upper()} CRC_OK={crc_str}")
                except Exception as e:
                    pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                    self.append_tv(self.tv_parsed, f"[R07-TOTALIZER-ERR] {e} payload={pl.hex().upper()}")
                i += 2 + lng
                continue
           # --- 0x38: Event (TRANS=0x01, LEN=0x02 → CODE + EXTRA) ---
            if cmd == 0x38:
                try:
                    pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                    crc_str = "True" if (ok is True) else ("False" if (ok is False) else "N/A")
                    code  = data[0] if lng >= 1 else 0x00
                    extra = data[1] if lng >= 2 else 0x00
                    code_map = {
                            0x10: "NOZZLE_OUT",
                            0x11: "NOZZLE_IN",
                            0x21: "TRIGGER_ON",
                            0x20: "TRIGGER_OFF",
                    }
                    desc = code_map.get(code, "UNKNOWN")
                    self.append_tv(self.tv_parsed,
                                   f"[R07-EVENT] payload={pl.hex().upper()} CRC_OK={crc_str}  "
                                   f"code=0x{code:02X} ({desc}) extra=0x{extra:02X}")
                except Exception as e:
                    pl = bytes([trans & 0xFF, lng & 0xFF]) + data
                    self.append_tv(self.tv_parsed, f"[R07-EVENT-ERR] {e} payload={pl.hex().upper()}")
                i += 2 + lng
                continue
            # Genel debug satırı (her transaction için)
            self.append_tv(
                self.tv_parsed,
                f"[R07-DC-TR] cmd=0x{cmd:02X} trans=0x{trans:02X} len={lng} data={data.hex().upper()}"
            )

            # --- TRANS=0x01 → Pump Status (DC1) ---
            if trans == 0x01 and len(data) >= 1:
                st = data[0]
                status_map = {
                    0x00: "NOT PROGRAMMED",
                    0x01: "RESET",
                    0x02: "AUTHORIZED",
                    0x04: "FILLING",
                    0x05: "FILLING COMPLETED",
                    0x06: "MAX AMOUNT/VOLUME",
                    0x07: "SWITCHED OFF",
                    0x0B: "PAUSED",
                }
                name = status_map.get(st, f"0x{st:02X}")
                # DC1 label
                try:
                    self.lbl_dc1.set_text(f"DC1: {name}")
                except Exception:
                    pass
                # Canonical state + LED
                canon = {
                    "NOT PROGRAMMED":      "NOT PROGRAMMED",
                    "RESET":               "RESET",
                    "AUTHORIZED":          "AUTHORIZED",
                    "FILLING":             "FILLING",
                    "FILLING COMPLETED":   "FILLING COMPLETED",
                    "MAX AMOUNT/VOLUME":   "MAX AMOUNT/VOLUME",
                    "SWITCHED OFF":        "SWITCHED OFF",
                    "PAUSED":              "SUSPENDED",
                }.get(name, "RESET")
                try:
                    self.on_pump_status(canon)
                except Exception:
                    pass
                self.append_tv(
                    self.tv_parsed,
                    f"[R07-DC1-STATUS-TR] cmd=0x{cmd:02X} status={name}"
                )
                # Satış durumu takibini (SALE_DIAG tetik dahil) ortak yardımcıya taşıdık:
                self._sale_update_on_state(canon)
                # HS: DC-TR içinden gelen ilk DC1(status) çerçevesini de
                # "pompa hazır" kabul et. (Özellikle 0x3A kayıtlarında
                # sadece DC1+DC3 geldiği saha senaryosunu kapsamak için.)
                if not getattr(self, "_hs_ok", False):
                    try:
                        self._hs_ok = True
                        self.append_tv(
                            self.tv_parsed,
                            "[HS] DC1(trans) görüldü → POMPA HAZIR"
                        )
                        # Komut butonlarını aç
                        self._set_controls_enabled(True)
                        # Auto POLL açıksa HB'yi devrede tut
                        try:
                            if (
                                getattr(self, "ser", None)
                                and self.ser.is_open
                                and getattr(self, "chk_auto_poll", None)
                                and self.chk_auto_poll.get_active()
                                and not getattr(self, "_hb_timer_id", None)
                            ):
                                self._hb_start()
                        except Exception:
                            pass

                        # HS meta bilgileri
                        try:
                            self._hs_ever_ok = True
                            self._hs_last_ok_ts = time.monotonic()
                        except Exception:
                            pass
                    except Exception:
                        # HS sırasında hata olursa en azından GUI çökmesin
                        pass

            # --- TRANS=0x02 → Filled Volume / Amount (DC2) ---
            elif trans == 0x02 and len(data) >= 8:
                vol_raw = _bcd4_to_int(data[0:4])
                amo_raw = _bcd4_to_int(data[4:8])
                # Mepsan protokolüne göre:
                #  - VOL: litre*100 (iki ondalık), örn. 34.50 L → 3450
                #  - AMO: para birimi*100 (iki ondalık)
                vol_l    = vol_raw / 100.0
                amo_unit = amo_raw / 100.0
                try:
                    # Normal metin alanı
                    self.lbl_dc2.set_text(f"DC2: VOL={vol_l:.2f} L  AMO={amo_unit:.2f}")
                    # Glade'deki büyük sayısal alan (lbllevel) yalın litre değeriyle güncellensin
                    if getattr(self, "_glade_level_label", None):
                        self._glade_level_label.set_text(f"{vol_l:.1f}")
                except Exception:
                    pass
                self.append_tv(
                   self.tv_parsed,
                    f"[R07-DC2-AMOUNT] VOL={vol_l:.2f} L (raw={vol_raw}) AMO={amo_unit:.2f} (raw={amo_raw})"
               )
                # Satış için son DC2 bilgisini hatırla
                try:
                    self._sale_last_vol_raw = vol_raw
                    self._sale_last_amo_raw = amo_raw
                    self._sale_last_vol_l = vol_l
                    self._sale_last_amo_unit = amo_unit
                    if getattr(self, "_sale_active", False):
                        self._sale_has_dc2 = True
                except Exception:
                    pass
                # Preset (Mod-A): gerçek DC2 VOL değeri hedefe ulaştığında otomatik STOP
                # Not: _preset_target_ml mililitre cinsinden tutuluyor.
                # DC2 VOL BCD ise litre*100 → ml karşılığı: vol_raw * 10
                try:
                    if (self._preset_target_ml is not None) and (not self._preset_stop_sent):
                        # Hedefi DC2 ham birimine çevir: target_ml / 10 = litre*100
                        target_raw = int(self._preset_target_ml / 10)
                        if vol_raw >= target_raw:
                            self.append_tv(
                                self.tv_parsed,
                                "[AUTO] preset hedefe ulaşıldı → STOP (DC2)"
                            )
                            try:
                                self._send_cd1(0x08)  # STOP
                                self._preset_stop_sent = True
                                self._hint_state_intent("FILLING COMPLETED")
                            except Exception as e:
                                self.append_tv(self.tv_tx, f"[TX-ERR] STOP@preset(DC2): {e}")
                except Exception:
                    pass
            # --- TRANS=0x03 → Nozzle & Price (DC3) ---
            # DC101 (cmd=0x65) bu bloktan geçmesin ki aşağıdaki özel DC101
            # decoder'ına düşebilsin.
            elif cmd != 0x65 and trans == 0x03 and len(data) >= 4:
                # İlk 3 byte fiyat, son byte NOZIO
                price_raw = _bcd4_to_int(b'\x00' + data[0:3])
                price_unit = price_raw / 1000.0  # örn. 1.234 formatı
                nozio = data[3]
                noz_no = nozio & 0x0F
                out = bool(nozio & 0x10)
                state = "OUT" if out else "IN"
                try:
                    self.lbl_dc3.set_text(
                        f"DC3: noz={noz_no} {state} price={price_unit:.3f}"
                    )
                except Exception:
                    pass
                self.append_tv(
                    self.tv_parsed,
                    f"[R07-DC3-NOZZLE] nozzle={noz_no} state={state} price={price_unit:.3f} "
                    f"raw_price={price_raw} NOZIO=0x{nozio:02X}"
                )
                # Gerçek nozzle durumunu GUI'de de güncelle
                try:
                    self.on_nozzle_event(out)
                except Exception:
                    pass

            # --- CMD=0x65, TRANS=0x03 → DC101 Volume Total Counters ---
            elif cmd == 0x65 and trans == 0x03 and len(data) >= 1 + 5 + 5 + 5:
                # data[0] = nozzle
                # data[1:6]  = TOTVOL (5 BCD, litre*100)
                # data[6:11] = TOTV1  (5 BCD)
                # data[11:16]= TOTV2  (5 BCD)
                noz = data[0]
                totvol_raw = _bcd5_to_int(data[1:6])
                totv1_raw  = _bcd5_to_int(data[6:11])
                totv2_raw  = _bcd5_to_int(data[11:16])
                try:
                    totvol = totvol_raw / 100.0
                    totv1  = totv1_raw  / 100.0
                    totv2  = totv2_raw  / 100.0
                    self.append_tv(
                        self.tv_parsed,
                        f"[DC101] nozzle={noz} TOTVOL={totvol:.2f}L "
                        f"TOTV1={totv1:.2f}L TOTV2={totv2:.2f}L"
                        f"[DC101] total_liter={totvol:.2f} total_amount={totv1:.2f}"
                    )
                    # DC101 yalnızca DC101 etiketini günceller; lblcounter’a DOKUNMAZ.
                    try:
                        if getattr(self, "lbl_dc101", None):
                            self.lbl_dc101.set_text(
                                f"DC101: total_liter={totvol:.2f}L total_amount={totv1:.2f}"
                            )
                    except Exception:
                        pass
                except Exception:
                    # Her ihtimale karşı sadece RAW logla; [R07-DC-TR] satırları zaten yazıldı.
                    pass

            # Diğer TRANS değerleri (şimdilik sadece log)
            # elif trans == 0x65:  # örn. total counters vs.
            #     ...

            i += 2 + lng

        if i != n:
            # payload sonunda artan byte varsa uyarı düş
            self.append_tv(
                self.tv_parsed,
                f"[R07-DC-TR] uyarı: payload sonunda {n - i} artan byte var"
            )

    # ---------------- Handshake yardımcıları ----------------
    def on_destroy(self, *_):
        self._shutting_down = True
        try:
            if self.reader: self.reader.stop()
        except: pass
        try:
            self._hb_stop()
        except: pass
        try:
            if self.ser and self.ser.is_open: self.ser.close()
        except: pass
        try:
            self._log("=== CONTROLLER STOP ===")
            if self._logf:
                self._logf.close()
        except Exception:
            pass
        Gtk.main_quit()

    # ---------- Heartbeat helpers ----------
    def _safe_source_remove(self, sid):
        """
        GLib.source_remove için güvenli sargı:
        - Yalnızca geçerli bir int kimlik geldiyse kaldır.
        - Yarış/çift-kaldırma durumlarında TypeError'ı önler.
        """
        try:
            if isinstance(sid, int) and sid > 0:
                GLib.source_remove(sid)
                return True
        except Exception as e:
            try:
                self.append_tv(self.tv_rx, f"[SER-DBG] source_remove err: {e}")
            except Exception:
                pass
        return False
    def _hb_start(self):
        try:
            if self._safe_source_remove(getattr(self, "_hb_timer_id", None)):
                self._hb_timer_id = None
        except Exception:
            pass
        # HB sayaç sıfırlama (log gürültü filtresi için)
        try:
            self._hb_tick_count = 0
        except Exception:
            pass
        # Yeni bir HB döngüsü başlarken post-close hata logunu tekrar aç
        try:
            self._post_close_err_logged = False
        except Exception:
            pass
        self._hb_touch()
        self._hb_timer_id = GLib.timeout_add(self._hb_interval_ms, self._hb_tick)

    def _hb_stop(self):
        if self._safe_source_remove(getattr(self, "_hb_timer_id", None)):
            self._hb_timer_id = None

    def _hb_touch(self):
        try:
            self._hb_last_activity = time.monotonic()
        except Exception:
            self._hb_last_activity = 0.0

    def _hb_tick(self):
        # Her çağrıda sayaç artır; sadece her 10. tikte log yaz (POLL gönderimi her tikte)
        cnt = getattr(self, "_hb_tick_count", 0) + 1
        self._hb_tick_count = cnt
        log_this = (cnt % 10 == 0)
        # Terminal durumlarda bile HB'yi KESME — geçici kopma/geri gelme senaryolarında
        # poll devam etmeli ki pompa geri döndüğünde yakalanabilsin.
        # (Davranış değişikliği: artık terminal state HB'yi durdurmaz.)
        terminal_states = ("FILLING COMPLETED", "SWITCHED OFF", "MAX AMOUNT/VOLUME", "NOT PROGRAMMED")
        if getattr(self, "_state", "") in terminal_states:
            try:
                if log_this:
                    self.append_tv(self.tv_tx, "[HB] terminal; polling continues")
            except Exception:
                pass
            # NOT: return yok — aşağıdaki MIN-POLL akışı aynen çalışır.
        if not (self.ser and self.ser.is_open):
            self._hb_stop()
            return False

        # Auto POLL checkbox'ı kapandıysa timer'ı durdur.
        if not (getattr(self, "chk_auto_poll", None) and self.chk_auto_poll.get_active()):
            self._hb_stop()
            return False

        # HB: min-POLL ile canlı tut (HS olsun olmasın canlılık kontrolü)
        try:
            self._send_min_poll()
            if log_this:
                self.append_tv(self.tv_tx, "[HB→MIN] POLL (50 20 FA)")
        except Exception as e:
            self.append_tv(self.tv_tx, f"[HB-ERR] MIN-POLL: {e}")
        return True

if __name__ == "__main__":
    # Builder crashâ€™lerini azaltan gÃ¼venli varsayÄ±mlar:
    os.environ.setdefault("NO_AT_BRIDGE","1")
    os.environ.setdefault("GDK_BACKEND","win32")
    win = MainWin()
    win.show_all()
    Gtk.main()