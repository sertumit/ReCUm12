#!/usr/bin/env bash
# mk-code-temp.sh — verilen dosyalari code_temp.txt icine blok blok birlestirir

set -u

out_file="code_temp.txt"

echo "Dosya adlarini/yollarini sira ile girin."
echo "Bitirmek icin bos satir birakip Enter'a basin."
echo

files=()
while true; do
  read -r -p "Dosya yolu: " path
  # Bos satir -> cik
  if [ -z "$path" ]; then
    break
  fi
  files+=("$path")
done

if [ "${#files[@]}" -eq 0 ]; then
  echo "Hic dosya girilmedi, cikiliyor."
  exit 0
fi

# code_temp.txt olustur / sifirla
> "$out_file"

for f in "${files[@]}"; do
  if [ ! -f "$f" ]; then
    echo "Uyari: '$f' dosyasi bulunamadi, atlaniyor." >&2
    continue
  fi

  {
    echo "FILE: $f"
    echo "----------------------------------------"
    echo
    cat -- "$f"
    echo
    echo
  } >> "$out_file"
done

echo "Bitti. Olusan dosya: $(pwd)/$out_file"
