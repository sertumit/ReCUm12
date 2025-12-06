# GIT WORKFLOW – ReCUm12

Bu doküman, ReCUm12 projesinde **sağlıklı, kayıpsız ve anlaşılır bir Git akışı** için kullanılacak standart çalışma disiplini anlatır.

---

## 0. Temel Kavramlar

- **`main` branch’i**
  - Her zaman **çalışan, stabil hat**.
  - Çökmeyen, derlenen, sahaya çıkabilecek kod burada tutulur.
- **`sprint-*` branch’leri**
  - Günlük/haftalık geliştirme yapılan çalışma dalları.
  - Örnek: `sprint-v12.210.02`, `sprint-v12.210.03` vb.
- **Tag’ler (etiketler)**
  - Belirli bir commit’i “sürüm” olarak işaretlemek için kullanılır.
  - Örnek: `v12.200.01`, `v12.210.01`, `v12.210.02`…

> **Altın kural:**  
> - Geliştirme → `sprint-*` branch’lerinde  
> - Stabil/Saha → `main`  
> - Sürüm etiketi → her zaman **güncel `main` commit’i** üzerine

---

## 1. Yeni Sprint / Çalışma Dönemi Başlatma

Yeni bir sprint’e, daima güncel `main` üzerinden başlanır.

```bash
cd ~/ReCUm12

# 1) main'e geç
git switch main

# 2) main'i remote ile senkronize et
git pull origin main

# 3) Yeni sprint dalını main'den oluştur ve ona geç
git switch -c sprint-v12.210.03

# 4) Remote'a ilk kez gönder (tracking kurulumu)
git push -u origin sprint-v12.210.03
```

Bundan sonra bu sprint boyunca yapılacak tüm geliştirmeler
**`sprint-v12.210.03`** üzerinde ilerler.

---

## 2. Günlük Çalışma Döngüsü

Gün içinde yapılacak işlerin standart pattern’i:

1. Depo durumunu kontrol et
2. Kod değiştir, test et
3. Anlamlı commit’lere böl
4. Remote’a push et

```bash
cd ~/ReCUm12

# 1) Aktif dal ve değişiklikler
git status

# 2) Dosyaları düzenle / ekle / sil, sonra tekrar bak
git status

# 3) Değişen dosyaları stage et
git add apps/recum12_app/src/AppRuntime.cpp         modules/core/src/PumpRuntimeState.cpp

# 4) Anlamlı bir mesaj ile commit et
git commit -m "PumpOff_PC sıralamasını GunOff'tan sonra olacak şekilde sadeleştir"

# 5) Remote'a gönder
git push
```

**Kurallar:**

- Mümkün olduğunca **küçük ve anlamlı commit** yap.
- “Çalışıyor, mantıklı bir adım” diyebileceğin her durumda commit at.
- Lokal commit’leri unutmadan düzenli olarak `git push` ile GitHub’a gönder.

---

## 3. Dal Değiştirirken Güvenli Akış

Branch (dal) değiştirirken **önce her zaman**:

```bash
git status
```

### 3.1. Çalışma dizini temizse

```text
On branch sprint-v12.210.03
nothing to commit, working tree clean
```

Bu durumda güvenle dal değiştirebilirsin:

```bash
git switch main
```

### 3.2. Değişiklikler varsa

```text
modified:   AppRuntime.cpp
untracked files:
  code_temp.txt
```

Bu noktada 3 seçenek var:

#### A) Değişiklikleri kaydetmek istiyorum → Commit

```bash
git add AppRuntime.cpp code_temp.txt
git commit -m "WIP: GunOff/PumpOff denemeleri"
git push
git switch main
```

#### B) Henüz commit etmek istemiyorum ama kaybolmasın → Stash

```bash
git stash push -u -m "WIP AppRuntime before switching to main"
git switch main
```

Sonra geri almak için:

```bash
git switch sprint-v12.210.03
git stash list
git stash apply stash@{0}   # veya ilgili stash ID'si
```

#### C) Değişiklikler çöpe gidebilir → Discard

```bash
# Track edilen dosyaları eski haline getir
git restore AppRuntime.cpp

# Untracked dosyaları elle sil
rm code_temp.txt
```

---

## 4. Farklı Makineler Arasında Çalışma (Raspi ↔ PC)

Zip dosyaları ile uğraşmak yerine, senkronizasyonu **her zaman GitHub üzerinden** yap.

### 4.1. Çalışmayı bıraktığın makinede (ör. Raspi)

```bash
cd ~/ReCUm12
git status          # temiz mi?
git push            # tüm commit'ler remote'a gitsin
```

### 4.2. Devam edeceğin makinede (ör. PC)

```bash
cd ~/ReCUm12
git fetch           # tüm branch/tag güncellemelerini al
git switch sprint-v12.210.03
git pull            # dalın son halini indir
```

> **Not:** Her zaman, değişikliğe başlamadan önce:
> ```bash
> git pull
> ```
> ile remote’tan güncel hali çek.

---

## 5. Sprint Bittiğinde: main’e Merge ve Sürüm Alma

Bir sprint dalı stabil hale geldiğinde ve sahaya çıkmaya hazır olduğunda:

### 5.1. Sprint dalının temiz ve push’lu olduğundan emin ol

```bash
cd ~/ReCUm12
git switch sprint-v12.210.03
git status
git push
```

### 5.2. main’e merge et

```bash
# main'e geç
git switch main

# main'i en son remote hali ile güncelle
git pull origin main

# sprint dalını main'e merge et
git merge --no-ff sprint-v12.210.03
```

- Çakışma çıkarsa, ilgili dosyaları VSCode’dan veya elle çöz,
- Sonra:

```bash
git add <çakışması çözülen dosyalar>
git commit      # gerekirse (otomatik merge commit oluşmamışsa)
git push origin main
```

### 5.3. main commit’ine sürüm tag’i koy

```bash
# Güncel main commit'ini etiketle
git tag -a v12.210.03 main -m "Release v12.210.03"

# Tag'i remote'a gönder
git push origin v12.210.03
```

> Önemli: Tag’i her zaman **güncel, sahaya çıkacak `main` commit’ine** koy.  
> Eski snapshot’lara tag takarsan GitHub’da beklediğin dosyaları göremezsin.

---

## 6. Tag’leri Yönetme

### 6.1. Yeni sürüm tag’i oluşturma

```bash
git tag -a v12.210.03 main -m "Release v12.210.03"
git push origin v12.210.03
```

### 6.2. Hatalı tag’i düzeltme (repo tek kullanıcıysa)

Yanlış commit’e tag attıysan:

```bash
# Lokal tag'i sil
git tag -d v12.210.03

# Remote tag'i sil
git push origin :refs/tags/v12.210.03

# Doğru commit'e (main) yeniden tag at
git tag -a v12.210.03 main -m "Fix tag to correct commit"
git push origin v12.210.03
```

---

## 7. Sorun Anında Teşhis Komutları

Ne zaman kafan karışsa veya “kaybettim mi?” hissi gelse, şu üç komut:

```bash
# 1) Hangi branch'tesin? Çalışma dizini temiz mi?
git status

# 2) Branch'ler ve remote ile ilişkileri
git branch -vv

# 3) Son commit'ler, branch ve tag bağlantıları (grafik)
git log --oneline --graph --decorate --all
```

Bu üç çıktıyla; hangi dalın nerede olduğu, hangi tag’in nereye takılı olduğu ve local/remote farkları netleşir.

---

## 8. Özet – Kısa Kural Listesi

- Geliştirmeye başlamadan önce:
  - `git switch sprint-...`
  - `git pull`
- Çalışma sırasında:
  - `git status` → `git add` → `git commit` → `git push`
- Dal değiştirirken:
  - `git status`
  - Temiz değilse: **commit / stash / discard**’dan birini seç
- Sprint bitince:
  - sprint dalını `main`e merge et
  - `main` üzerinde sürüm tag’i yarat ve push et
- Makine değiştirirken:
  - Eski makinede `git push`
  - Yeni makinede `git fetch`, `git switch`, `git pull`

Bu dosya, ReCUm12 için **standart Git çalışma rehberi** olarak kullanılabilir.
