#!/usr/bin/env bash
# sha-clip.sh — Linux/Raspberry Pi uyumlu, kRevNo & arama satir numaralari ile

set -u

if [ "$#" -eq 0 ]; then
  echo "Kullanim: ./sha-clip.sh FILE [FILE ...] | DIR" >&2
  exit 1
fi

# --- Çikti modu secimi ------------------------------------------------------
read -r -p "Sadece Çapalar? (y/N): " only_anchors_ans
case "$only_anchors_ans" in
  [yY]) only_anchors=1 ;;
  *)    only_anchors=0 ;;
esac

# --- Klavyeden arama parametrelerini al ------------------------------------
read -r -p "Kac adet arama parametresi gireceksiniz? " anchor_count
echo "Arama stringlerini tek satirda, virgülle ayrilmis gir (ornegin: a,b,c):"
read -r -p "Aramalar: " anchors_raw
echo

IFS=',' read -ra anchors <<< "$anchors_raw"

# Kullanilacak arama sayisini sinirla (fazlasi girildiyse ignore)
used_count=${#anchors[@]}
if [ "$anchor_count" -lt "$used_count" ]; then
  used_count=$anchor_count
fi

# --- Dosyalari topla --------------------------------------------------------
files=()

for p in "$@"; do
  if [ -f "$p" ]; then
    files+=("$p")
  elif [ -d "$p" ]; then
    # Alt dizinlerdeki tum dosyalar
    while IFS= read -r -d '' f; do
      files+=("$f")
    done < <(find "$p" -type f -print0 2>/dev/null)
  else
    echo "Yol bulunamadi: $p" >&2
  fi
done

if [ "${#files[@]}" -eq 0 ]; then
  echo "Islenecek dosya yok." >&2
  exit 1
fi

# --- Isle -------------------------------------------------------------------
out=""

for full in "${files[@]}"; do
  name=$(basename "$full")

  # SHA256 (kucuk harf)
  actHash=$(sha256sum "$full" | awk '{print tolower($1)}')

  # Satir sayimi: wc -l (LF sayisi) + sondaki LF yoksa +1
  lineCount=$(wc -l <"$full")
  if [ -s "$full" ]; then
    lastchar=$(tail -c1 "$full" | od -An -t u1)
    lastchar=${lastchar//[[:space:]]/}
    if [ "$lastchar" != "10" ]; then
      lineCount=$((lineCount + 1))
    fi
  fi

  # APP_VERSION satiri (kRevNo üstünden araniyor)
  appVersionLine='-'
  if grep -qE 'kRevNo[[:space:]]*=' "$full"; then
    appVersionLine=$(grep -nE 'kRevNo[[:space:]]*=' "$full" | head -n1 | cut -d: -f1)
  fi

  # Baslik / meta kismi (moda gore)
  out+="FILE: $full"$'\n'
  if [ "$only_anchors" -eq 0 ]; then
    out+="ACTUAL_SHA=$actHash"$'\n'
    out+="LINES=$lineCount"$'\n'
    out+="APP_VERSION satiri:$appVersionLine"$'\n'
  fi
  out+="ÇAPALAR:"$'\n'

  # Her arama string'i icin tum satir numaralarini bul
  for ((i=0; i<used_count; i++)); do
    pattern=${anchors[i]}

    # Bos pattern gelirse direkt '-' yaz
    if [ -z "$pattern" ]; then
      out+="  $pattern ->- "$'\n'
      continue
    fi

    # Tüm eşleşmeleri topla
    mapfile -t hits < <(grep -nF "$pattern" "$full" 2>/dev/null || true)

    if [ "${#hits[@]}" -eq 0 ]; then
      lines='-'
    else
      nums=()
      for h in "${hits[@]}"; do
        nums+=( "${h%%:*}" )
      done
      IFS=$'\n' sorted=($(printf '%s\n' "${nums[@]}" | sort -n))
      unset IFS
      # 405,1560,2430 formatina cevir
      lines=$(printf '%s,' "${sorted[@]}")
      lines=${lines%,}
    fi

    out+="  $pattern -> $lines"$'\n'
  done

  out+=$'\n'
done

# --- Panoya yaz / cikti ver -------------------------------------------------
# Sondaki fazladan newline'i kirpmaya calis
out_trimmed="$out"
while [[ "$out_trimmed" == *$'\n' ]]; do
  out_trimmed="${out_trimmed%$'\n'}"
done

# Bos mu kontrolu (sadece whitespace ise bos say)
if [ -n "$(printf '%s' "$out_trimmed" | tr -d '[:space:]')" ]; then
  printf '%s\n' "$out_trimmed"
  if command -v xclip >/dev/null 2>&1; then
    printf '%s' "$out_trimmed" | xclip -selection clipboard
    echo "Panoya kopyalandi."
  elif command -v xsel >/dev/null 2>&1; then
    printf '%s' "$out_trimmed" | xsel --clipboard --input
    echo "Panoya kopyalandi."
  else
    echo "Uyari: xclip/xsel bulunamadi, panoya kopyalanamadi." >&2
  fi
else
  echo "Uretilecek cikti yok."
fi
