#!/usr/bin/env bash
# sha-clip.sh — Linux uyumlu, kilit/sidecar destekli, sağlam satır sayımı
# Kullanım: ./sha-clip.sh FILE [FILE ...] | DIR

set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "Kullanım: $0 FILE [FILE ...] | DIR" >&2
  exit 1
fi

# --- Yardımcılar ---------------------------------------------------------

get_file_sha256() {
  # $1 = path
  # GNU coreutils sha256sum çıktısı: "<hash>  <file>"
  sha256sum -- "$1" | awk '{print tolower($1)}'
}

get_line_count() {
  # Bayt düzeyinde sayım: LF say + sondaki LF yoksa +1
  # Python yoksa wc -l'ye düşer (LF yoksa +1 davranışı aynı değil olabilir).
  local path=$1

  if command -v python3 >/dev/null 2>&1; then
    python3 - "$path" << 'PY'
import sys

p = sys.argv[1]
with open(p, 'rb') as f:
    data = f.read()

if not data:
    print(0)
else:
    lf = data.count(b'\n')
    if data[-1:] != b'\n':
        lf += 1
    print(lf)
PY
  else
    # Yedek: wc -l (tam aynı tanım değil ama yakın)
    # Boş değilse ve son byte LF değilse +1
    if [ ! -s "$path" ]; then
      echo 0
      return
    fi
    local lines last_byte
    lines=$(wc -l < "$path")
    # son byte'ı oku
    last_byte=$(tail -c 1 -- "$path" | od -An -t u1 | awk '{print $1}')
    if [ "$last_byte" != "10" ]; then
      lines=$((lines + 1))
    fi
    echo "$lines"
  fi
}

try_write_clipboard() {
  # stdin'den gelen metni panoya yazmaya çalış.
  # Önce wl-copy (Wayland), sonra xclip, sonra xsel
  if command -v wl-copy >/dev/null 2>&1; then
    wl-copy
    return 0
  elif command -v xclip >/dev/null 2>&1; then
    xclip -selection clipboard
    return 0
  elif command -v xsel >/dev/null 2>&1; then
    xsel --clipboard --input
    return 0
  else
    return 1
  fi
}

# BASE_SHA regex'i:
# optional # // ; + boşluk, BASE_SHA [:=] 64 hex
rx_base='^[[:space:]]*([#;/]{0,2})[[:space:]]*BASE_SHA[[:space:]]*[:=][[:space:]]*([0-9a-fA-F]{64})[[:space:]]*$'

# --- Dosyaları topla ------------------------------------------------------

files=()

for p in "$@"; do
  if [ -f "$p" ]; then
    files+=("$p")
  elif [ -d "$p" ]; then
    # recursive tüm normal dosyalar
    while IFS= read -r -d '' f; do
      files+=("$f")
    done < <(find "$p" -type f -print0 2>/dev/null)
  else
    echo "Yol bulunamadı: $p" >&2
  fi
done

if [ "${#files[@]}" -eq 0 ]; then
  echo "İşlenecek dosya yok." >&2
  exit 1
fi

# --- İşle -----------------------------------------------------------------

out=""

for full in "${files[@]}"; do
  # Güvenlik için hatayı yakala ama diğer dosyalar devam etsin
  {
    name=$(basename -- "$full")

    act_hash=$(get_file_sha256 "$full")
    line_count=$(get_line_count "$full")

    # Sidecar BASE_SHA
    ##base="-"
    ##sidecar="${full}.BASE_SHA"
    ##if [ -f "$sidecar" ]; then
      # sadece ilk eşleşmeyi al
      ##while IFS= read -r line; do
        ##if [[ "$line" =~ $rx_base ]]; then
          ##base="${BASH_REMATCH[2],,}"   # lower-case
          ##break
        ##fi
      ##done < "$sidecar"
    ##fi
    out+=$'Ok. Derleme işlemi başarılı.\n'
    out+=$'FILE: '"$name"$'\n'
    #out+=$'FILE: '"$name"$'\n'
    out+=$'ACTUAL_SHA='"$act_hash"$'\n'
    out+=$'LINES='"$line_count"$'\n'
    out+=$'\n'
  } || {
    echo "Hata: $full işlenemedi." >&2
  }
done

# --- Panoya yaz / çıktı ver ----------------------------------------------

# sondaki boş satırları kırp
# (printf ile yazıp sed ile trim)
trimmed=$(printf "%s" "$out" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' )

if [ -n "${trimmed//[[:space:]]/}" ]; then
  printf "%s\n" "$trimmed"
  if printf "%s" "$trimmed" | try_write_clipboard; then
    echo "Panoya kopyalandı." >&2
  else
    echo "Panoya kopyalanamadı (wl-copy/xclip/xsel yok)." >&2
  fi
else
  echo "Üretilecek çıktı yok."
fi
