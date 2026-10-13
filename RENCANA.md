# Rencana Pengembangan Nxzilla

**Jembatan terminal ke server remote.**
User cukup membuka terminal di folder proyek, jalankan `nxzilla`, memasukkan
konfigurasi SSH/FTP (host, username, password), lalu **berkas proyek remote
masuk ke folder kerja (cd) tersebut**. Perubahan dikirim balik ke server lewat
perintah.

Lahir dari kesulitan kerja remote: dulu user harus "mendarat mandiri" — pindah
dari FileZilla FTP client ke folder Explorer secara manual. Nxzilla mengangkat
alur itu menjadi satu perintah terminal dengan tampilan TUI, serupa
[`NxPort`](../../CLI/NxPort) dan [`NxfireDB`](../../Database/NxfireDB).

---

## 0. Apa Nxzilla

```
        Terminal user (folder kerja saat ini, mis. D:\Website)
                        |
                        |  PS D:\Website> nxzilla
                        v
   +-----------------------------------------------+
   |  Nxzilla  (CLI + TUI, C++, Windows & Linux)   |
   |                                               |
   |   konfigurasi ssh/ftp  ->  koneksi ke server  |
   |   tarik berkas proyek  ->  taruh di cd        |
   |   pantau perubahan     ->  push / pull        |
   +-----------------------------------------------+
                        |
        +---------------+---------------+
        |                               |
   SSH / SFTP (OpenSSH)            FTP / FTPS
        |                               |
        v                               v
   server remote (mis. VPS nxdom)   server ftp hosting
```

Nxzilla **bukan** FileZilla. Tidak ada dua panel (lokal vs remote), tidak ada
drag-and-drop. Yang ada satu alur:

1. User buka terminal di folder proyek.
2. Jalankan `nxzilla` → masukkan konfigurasi `ssh` atau `ftp` (host, user,
   password, path remote).
3. Berkas proyek remote **ditarik masuk ke folder kerja saat ini** (inilah
   "cd" pada README). Struktur folder remote dipertahankan.
4. User bekerja seperti biasa dengan berkas lokal (edit di editor favorit).
5. Perubahan dikirim balik ke server dengan perintah (`nxzilla push`), atau
   menarik perubahan server dengan `nxzilla pull`.

> Referensi manual yang ingin digantikan: folder seperti
> `D:\info@nxdom.org\ssh` — dulu user menyusun sendiri folder bernama
> `<user>@<host>\<protokol>` lalu memindahkan berkas ke sana. Nxzilla
> mengotomatiskan penamaan, koneksi, dan sinkronisasi itu.

### Dua lingkungan

Nxzilla hidup di **dua konteks** yang sama-sama penting:

| Konteks | Kapan | Yang terjadi |
| :--- | :--- | :--- |
| **Lingkungan proyek** | `nxzilla` dijalankan di dalam folder proyek (ada `.nxzilla.json`) | Sinkronisasi proyek itu (pull/push/status) |
| **Lingkungan global** | `nxzilla` dijalankan di luar proyek | Menu **daftar semua proyek** yang memakai Nxzilla, untuk dibuka & dikelola |

Lingkungan global adalah "beranda" Nxzilla: kamu bisa melihat **proyek apa saja
yang memakai Nxzilla**, dan membuka salah satunya dari situ. Lihat bagian 3.

### Mengapa bentuk ini berguna

- Tidak perlu lagi buka aplikasi GUI FTP terpisah; semua dari terminal.
- Folder kerja = proyek remote. Editor/IDE/build tool lokal langsung bekerja.
- Satu berkas konfigurasi per proyek; tidak perlu menghafal host/path.
- Satu beranda global untuk mengelola seluruh proyek remote.
- **Jalan di Windows dan Linux** dari satu basis kode (lihat 6.6).
- Berbasis perintah → bisa dipakai di script, CI, dan terminal mana pun.

---

## 1. Konfigurasi

### 1.1 Profil koneksi global

Disimpan di:

- `%APPDATA%\Nxzilla\connections.json` (Windows)
- `~/.config/nxzilla/connections.json` (Linux)

```jsonc
{
  "profiles": [
    {
      "name": "nxdom-ssh",
      "protocol": "ssh",            // "ssh" | "ftp" | "ftps"
      "host": "nxdom.org",
      "port": 22,                   // ssh: 22, ftp: 21
      "user": "info",
      "password": "••••••",         // disimpan ter-obfuscate, lihat 1.3
      "key": "~/.ssh/id_ed25519",   // opsional untuk ssh (ganti password)
      "remotePath": "/var/www/nxdom"
    },
    {
      "name": "hosting-ftp",
      "protocol": "ftp",
      "host": "ftp.example.com",
      "port": 21,
      "user": "deploy",
      "password": "••••••",
      "remotePath": "/public_html"
    }
  ]
}
```

### 1.2 Konfigurasi per-proyek

File `.nxzilla.json` di folder kerja (dibuat saat `nxzilla` pertama dijalankan
di folder itu):

```jsonc
{
  "profile": "nxdom-ssh",        // rujuk nama di connections.json
  "remotePath": "/var/www/nxdom",// boleh menimpa remotePath profil
  "lastSync": "2026-10-13T10:20:00Z",
  "ignore": [".git/", "node_modules/", "*.log", ".env"]
}
```

- `.nxzilla.json` menentukan: proyek ini terhubung ke profil mana dan folder
  remote mana. Ini yang membuat `nxzilla` di folder itu langsung tahu harus
  menarik/mendorong apa. Keberadaan berkas ini jugalah yang menandai sebuah
  folder sebagai **proyek Nxzilla**.
- `ignore` — pola yang dilewati saat push/pull (default: `.git/`,
  `node_modules/`, `vendor/`, `*.log`).

### 1.3 Registry proyek global

Agar lingkungan global (bagian 3) tahu ada proyek apa saja, setiap proyek
tercatat di:

- `%APPDATA%\Nxzilla\projects.json` (Windows)
- `~/.config/nxzilla/projects.json` (Linux)

```jsonc
{
  "projects": [
    {
      "name": "Website nxdom",
      "localPath": "D:\\Website",
      "profile": "nxdom-ssh",
      "remotePath": "/var/www/nxdom",
      "lastSync": "2026-10-13T10:20:00Z",
      "lastStatus": "synced"       // synced | dirty | error | unknown
    },
    {
      "name": "Toko Online",
      "localPath": "D:\\Projects\\toko",
      "profile": "hosting-ftp",
      "remotePath": "/public_html",
      "lastSync": "2026-10-13T09:05:00Z",
      "lastStatus": "dirty"
    }
  ]
}
```

- Entri **ditambah otomatis** saat `nxzilla` pertama menyetel sebuah folder
  sebagai proyek (baik dari dalam proyek, maupun dari menu global "Tambah").
- Bisa juga diisi otomatis dengan `nxzilla scan <folder>` — Nxzilla menelusuri
  folder dan mendaftarkan semua yang punya `.nxzilla.json`.
- `lastStatus` diperbarui setiap kali proyek itu disinkron/di-push. Status
  "hidup" (terhubung/tidak) dicek saat menu global dibuka atau diminta user.
- Menghapus entri registry **tidak** menghapus folder proyek — hanya berhenti
  melacaknya.

### 1.4 Keamanan kredensial

- Password **tidak** disimpan sebagai teks polos. Minimal: disamarkan +
  peringatan jelas. Ideal: gunakan kredensial manager OS
  (Windows Credential Manager / `libsecret` di Linux).
- Prioritas: (a) SSH key, (b) tanya password tiap sesi, (c) simpan
  ter-obfuscate bila user memaksa "ingat".
- Di lingkungan non-interaktif (script), baca dari env var
  `NXZILLA_PASSWORD`.

---

## 2. Cara Pakai & Sintaks Perintah

### 2.1 Alur utama (dari README)

```powershell
PS D:\Website> nxzilla
```

Pertama kali di folder ini:
1. Cek `.nxzilla.json`. Bila tidak ada → tanya: **jadikan folder ini proyek
   Nxzilla?** (pilih/buat profil koneksi, tentukan path remote).
2. Saat proyek dibuat → otomatis **daftarkan ke registry** (1.3).
3. Tampilkan TUI dashboard, mulai **menarik berkas proyek remote** ke
   `D:\Website` (dengan progress).
4. Setelah sinkron, dashboard menampilkan status: jumlah berkas, terakhir
   sinkron, dan daftar perubahan bila ada.

Menjalankan lagi setelahnya:
- Nxzilla membandingkan berkas lokal vs remote (mtime/ukuran/hash), lalu
  memberi tahu mana yang berbeda dan menawarkan tarik/kirim.

### 2.2 Daftar perintah

```sh
nxzilla                      # dalam proyek: sinkron proyek; di luar: ruang global
nxzilla projects             # paksa buka ruang global (daftar proyek)
nxzilla open <nama|path>     # buka/sinkron proyek terdaftar dari mana saja
nxzilla add                  # daftarkan folder kini sebagai proyek (+ profil)
nxzilla remove <nama>        # hapus dari registry (folder tidak dihapus)
nxzilla scan [folder]        # cari folder ber-.nxzilla.json lalu daftarkan
nxzilla pull                 # tarik perubahan dari server ke folder kerja
nxzilla push                 # kirim perubahan folder kerja ke server
nxzilla status               # tampilkan status sinkronisasi (tanpa mengubah)
nxzilla ls [path]            # daftar isi folder remote (tanpa menarik berkas)
nxzilla use <nama-profil>    # set profil untuk folder kerja saat ini
nxzilla ignore <pola>        # tambah pola ignore ke .nxzilla.json
nxzilla --dry-run push       # simulasi push, tampilkan berkas yang akan dikirim
nxzilla --help               # bantuan
nxzilla --version            # versi
```

Contoh sesi nyata:

```powershell
PS D:\Website> nxzilla add
  Protokol (ssh/ftp): ssh
  Host: nxdom.org
  Port [22]: 
  User: info
  Password: ********
  Path remote [/var/www]: /var/www/nxdom
  Simpan sebagai profil [nxdom-ssh]: 
  -> Folder ini didaftarkan sebagai proyek "Website nxdom".

PS D:\Website> nxzilla
  NXZILLA  v0.1.0
  Profil : nxdom-ssh  (ssh info@nxdom.org:22)
  Remote : /var/www/nxdom
  Lokal  : D:\Website
  Menarik berkas... [############------] 62%  (124/200 berkas)
```

### 2.3 Tampilan TUI proyek (gaya NxPort / NxfireDB)

Satu layar dashboard, keyboard-driven, tanpa panel lokal/remote terpisah:

```
+-- NXZILLA v0.1.0 ----------------------------------------------+
|  info@nxdom.org : ssh :22        TERHUBUNG  |  latency 38 ms  |
|  Remote /var/www/nxdom       Lokal D:\Website                  |
|  Sinkron terakhir: 2 menit lalu (200 berkas, 3 berubah)        |
+----------------------------------------------------------------+
|  Status sinkronisasi:                                          |
|    M  public_html/index.php         lokal lebih baru           |
|    A  public_html/uploads/foto.jpg  hanya di server            |
|    D  public_html/lama.txt          hanya di lokal             |
+----------------------------------------------------------------+
|  [p] push  [l] pull  [s] status  [g] global  [c] config  [q]   |
+----------------------------------------------------------------+
```

Kontrol:

| Tombol | Aksi |
| :--- | :--- |
| `p` | Push (kirim perubahan lokal ke server) dengan konfirmasi |
| `l` | Pull (tarik perubahan server ke lokal) |
| `s` | Segarkan status sinkronisasi |
| `g` | Buka **ruang global** (daftar proyek, bagian 3) |
| `c` | Buka konfigurasi (profil / path / ignore) |
| `↑`/`↓` | Telusuri daftar perubahan |
| `q` / `Esc` | Keluar |

Mode non-interaktif untuk script: `--dry-run`, `--once`, `--no-color`,
`--quiet`.

---

## 3. Ruang Global & Manajemen Proyek

Ini **beranda** Nxzilla. Dijalankan saat tidak berada di dalam folder proyek
(tidak ada `.nxzilla.json`), atau lewat `nxzilla projects` / tombol `g`.

```
+-- NXZILLA v0.1.0 — Ruang Global --------------------------------+
|  Profil koneksi: 2        Proyek terdaftar: 3                   |
+------------------------------------------------------------------+
|  Daftar Proyek:                                                  |
|   > Website nxdom    D:\Website        nxdom-ssh    synced   2m  |
|     Toko Online      D:\Projects\toko  hosting-ftp  dirty    1j  |
|     Blog             D:\Blog           nxdom-ssh    error    -   |
+------------------------------------------------------------------+
|  [Enter]buka [p]push [l]pull [a]tambah [d]hapus [s]profil [r] [q]|
+------------------------------------------------------------------+
```

Kontrol:

| Tombol | Aksi |
| :--- | :--- |
| `↑`/`↓` | Pilih proyek |
| `Enter` | Buka proyek (lihat catatan di bawah) |
| `p` / `l` | Push / pull proyek terpilih tanpa masuk ke foldernya |
| `s` | Segarkan status semua proyek (uji koneksi ringan) |
| `a` | Tambah proyek: arahkan ke folder atau buat proyek baru |
| `d` | Hapus dari registry (folder **tidak** dihapus, dengan konfirmasi) |
| `c` | Kelola profil koneksi (`connections.json`) |
| `r` | Refresh daftar |
| `q` / `Esc` | Keluar |

**Catatan penting (keterbatasan shell).** Sebuah program tidak bisa mengubah
direktori shell induknya. Maka "buka proyek" berarti salah satu:

1. **Masuk mode proyek** di dalam sesi Nxzilla (dashboard proyek terbuka,
   sinkronisasi jalan) — perilaku default.
2. **Lompat ke folder** dengan membuka **subshell** baru di `localPath`
   (Windows: `pwsh`/`cmd` di folder itu; Linux: `$SHELL`), atau cetak path-nya
   agar user bisa `cd` sendiri (mis. `nxzilla open tokо --print-cd`).

Opsi ini bisa dipilih lewat pengaturan, agar ekspektasi tidak salah.

**Fungsi manajemen lain:**

| Perintah | Fungsi |
| :--- | :--- |
| `nxzilla projects` | Buka ruang global dari mana saja |
| `nxzilla scan D:\Projects` | Telusuri & daftarkan semua proyek Nxzilla di folder itu |
| `nxzilla open <nama\|path>` | Buka proyek tanpa harus `cd` dulu |
| `nxzilla remove <nama>` | Berhenti melacak proyek |
| `nxzilla status --all` | Ringkasan status semua proyek (untuk script/CI) |

Indikator status proyek:

- `synced` — lokal & remote sama (menurut sinkron terakhir).
- `dirty` — ada perubahan lokal yang belum di-push.
- `error` — gagal terhubung / sinkron terakhir gagal.
- `unknown` — belum pernah disinkron sejak terdaftar.

---

## 4. Fitur Tambahan (nilai jual)

### 4.1 Sinkronisasi dua arah cerdas

- Bandingkan dengan kombinasi **ukuran + waktu + hash** untuk memutuskan arah
  perubahan (hindari unduh berkas yang tidak berubah).
- Deteksi konflik (berkas berubah di dua sisi) → minta user memilih.
- Mode `--force` untuk menimpa tanpa tanya.

### 4.2 Statistik & ringkasan proyek

Setelah sinkron, tampilkan ringkasan seperti NxPort: jumlah berkas, total
ukuran, ukuran yang diunduh/diunggah, waktu tempuh, kecepatan rata-rata.

### 4.3 Riwayat & batalkan (undo)

- Simpan snapshot ringan (hash + daftar berkas) sebelum push → bisa `rollback`.
- Riwayat sinkron per proyek.

### 4.4 Sinkronisasi selektif

- `nxzilla push public_html/` → hanya folder tertentu.
- Dukungan `ignore` (lihat 1.2) untuk melewati `.git`, `node_modules`, dst.

### 4.5 Notifikasi perubahan server

- Mode `watch` (opsional): pantau perubahan remote berkala dan beri tahu user
  tanpa menarik semuanya.

### 4.6 Integrasi editor/IDE

- `nxzilla edit <file>` → tarik berkas, buka di `$EDITOR`, unggah balik saat
  simpan. (Terinspirasi cara kerja remote edit yang ringan.)

---

## 5. Struktur Repo

Mengikuti pola NxPort (sederhana) — target awal satu-dengan-sedikit berkas:

```
Nxzilla/
  nxzilla.cpp               inti: koneksi + sinkron + registry + TUI (single file, awal)
  include/nxzilla/          (fase lanjut) header bila dipecah modular
  src/                      (fase lanjut) Sync.cpp, Ssh.cpp, Ftp.cpp, Tui.cpp, Registry.cpp
  build.bat                 build Windows (MSVC)
  build.sh                  build Linux (g++)
  nxzilla.exe / nxzilla     hasil build
  connections.example.json  contoh profil koneksi
  .nxzilla.json.example     contoh konfigurasi proyek
  projects.example.json     contoh registry proyek global
  README.md                 ringkasan proyek
  RENCANA.md                dokumen ini
  LICENSE                   MIT
```

> Catatan: `NxfireDB` memakai CMake + multi-folder karena ia library + server +
> SDK. Nxzilla cukup aplikasi CLI; start dengan **satu berkas** (`nxzilla.cpp`)
> seperti `NxPort`, lalu pecah modular bila sudah tumbuh.

---

## 6. Komponen Teknis Inti

### 6.1 Koneksi & transfer

| Protokol | Windows | Linux | Tanpa dependensi? |
| :--- | :--- | :--- | :--- |
| SSH/SFTP | subprocess `ssh`/`sftp`/`scp` (OpenSSH bawaan Win10+) | subprocess `ssh`/`sftp`/`scp` | Ya |
| SSH/SFTP (alt) | `libssh2` (static) | `libssh2` | Perlu build |
| FTP/FTPS | `WinINet` (bawaan) | `libcurl` / socket mentah | WinINet: ya |

**Pilihan utama (MVP): subprocess OpenSSH.** Alasannya:
- Mengikuti filosofi NxPort: tanpa dependensi pihak ketiga.
- OpenSSH client sudah tersedia di Windows 10/11 dan semua distro Linux.
- Autentikasi (password, key, agent) ditangani OpenSSH — tidak perlu
  mengimplementasi protokol SSH dari nol.
- Mendukung mode batch (`sftp -b -`) untuk skrip transfer.

**Alternatif `libssh2`** dipertimbangkan bila ingin kontrol penuh atas
sesi/koneksi persisten tanpa proses anak (lihat Keputusan Tersisa).

### 6.2 Mesin sinkronisasi

- Pindai pohon berkas lokal dan remote (SFTP `readdir` / FTP `LIST` /
  `ssh find -printf`).
- Klasifikasi tiap berkas: `sama`, `M` (modifikasi), `A` (hanya remote),
  `D` (hanya lokal).
- Tentukan aksi: pull / push / skip / konflik.
- Hormati `ignore`.
- Eksekusi transfer dengan progress byte (parse output `scp`/`sftp` atau
  hitung dari jumlah berkas).

### 6.3 Registry & ruang global

- Muat/tulis `projects.json` (bagian 1.3) tiap kali proyek dibuka/dibuat.
- Deteksi konteks: telusuri `.nxzilla.json` dari cwd ke atas. Ketemu → mode
  proyek; tidak → mode global.
- Uji status proyek secara *lazy* (hanya yang tampil di layar) agar menu
  global tetap cepat walau proyeknya banyak.
- Operasi dari menu global (`p`/`l`) memakai profil & path dari entri registry
  tanpa mengubah cwd — jadi bisa mengelola proyek dari mana saja.

### 6.4 TUI (rerender bebas kedip)

- Tiru pola NxPort: render ulang hanya saat data/tombol berubah, tulis satu
  frame, aktifkan *synchronized output* + VT processing di Windows
  (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`).
- Input tombol non-blocking; transfer berjalan di thread terpisah sementara
  TUI tetap responsif.
- Satu kerangka TUI dipakai baik untuk mode proyek maupun ruang global.

### 6.5 Penanganan path

- Normalisasi pemisah path (`\` vs `/`), dukungan path remote absolut/relatif.
- Sanitasi nama berkas agar aman terhadap traversal (`..`).

### 6.6 Dukungan lintas platform (Windows & Linux)

**Satu basis kode, dua build script** (`build.bat` untuk MSVC, `build.sh`
untuk g++). Prinsipnya sama seperti NxPort: fitur dipisah di balik antarmuka
yang sama, detail platform hanya hadir di lapisan tipis.

| Aspek | Windows | Linux |
| :--- | :--- | :--- |
| Backend SSH | subprocess OpenSSH (`ssh`/`sftp`/`scp`) | subprocess OpenSSH |
| Backend FTP | `WinINet` (bawaan) | `libcurl` / socket |
| Terminal | VT processing + `GetConsoleScreenBufferInfo` | ANSI + `ioctl(TIOCGWINSZ)` |
| Deteksi ukuran layar | `GetConsoleScreenBufferInfo` | `ioctl(TIOCGWINSZ)` |
| Config | `%APPDATA%\Nxzilla\` | `~/.config/nxzilla/` |
| Pemisah path | `\` (diterima `/`) | `/` |
| Editor default | `%EDITOR%` (fallback `notepad`) | `$EDITOR` (fallback `vi`) |

Ketentuan lintas platform:

- **Deteksi OS saat kompilasi** (`_WIN32`) untuk jalur kode khusus; sisanya
  berbagi logika (registry, sinkron, TUI, parser konfigurasi).
- **Output UTF-8** di kedua OS; di Windows aktifkan code page + VT.
- **Tidak bergantung pada shell tertentu** saat memanggil subprocess (pakai
  path executable langsung, bukan `cmd`/`sh`).
- **Nama/config path** diselesaikan lewat helper `ConfigDir()` yang mengembalikan
  lokasi berbeda per-OS.
- **CI uji dua platform**: build + uji dasar (`--version`, `--help`, registry)
  harus hijau di Windows **dan** Linux.

---

## 7. Roadmap per Fase

> **Status v1.0.0 (rilis).** Berikut yang sudah selesai dan teruji:
> Fase 0 (build Windows **dan** Linux hijau), Fase 1 (profil & konfigurasi),
> Fase 2 (SSH/SFTP — routing terverifikasi; transfer penuh menunggu server uji),
> Fase 3 (FTP/FTPS — teruji penuh), Fase 4 (mesin sinkron, ignore, konflik +
> `--force`, `--dry-run`, `--checksum`), Fase 5 (registry, `add`/`scan`/`use`/
> `ignore`/`remove`, ruang global), Fase 6 (TUI dua konteks + progress transfer),
> dan Fase 7 sebagian (obfuskasi kredensial + `NXZILLA_PASSWORD`).
> **Belum:** OS keyring, `watch`/`edit`, statistik kecepatan, onboarding folder,
> uji transfer SSH nyata.

### Fase 0 — Bootstrap
- Kerangka repo, `build.bat`, `build.sh`, `.gitignore`, `LICENSE`.
- `connections.example.json`, `.nxzilla.json.example`, `projects.example.json`.
- **DoD:** build hijau di **Windows (MSVC) dan Linux (g++)** — binari kosong,
  `nxzilla --version` dan `nxzilla --help` jalan di kedua OS.

### Fase 1 — Profil & konfigurasi
- Baca/tulis `connections.json` dan `.nxzilla.json`.
- Perintah `nxzilla add`, `nxzilla use`, `nxzilla config`.
- Tanya input interaktif (host/user/password/path) tanpa echo password.
- **DoD:** bisa menyimpan profil ssh & ftp, dan menautkannya ke folder kerja.

### Fase 2 — Koneksi SSH/SFTP
- Bungkus OpenSSH: test koneksi, `ls` remote, tarik berkas (`sftp -b` /
  `scp`).
- Tangani error: host tak dikenal, auth gagal, timeout, host key baru.
- **DoD:** `nxzilla ls` menampilkan isi folder server SSH nyata; `nxzilla`
  menarik proyek ke folder kerja.

### Fase 3 — Koneksi FTP/FTPS
- FTP via WinINet (Windows) / libcurl (Linux).
- `ls`, pull, push.
- **DoD:** alur yang sama seperti Fase 2 berhasil untuk server FTP.

### Fase 4 — Mesin sinkron & perintah pull/push
- Bandingkan lokal vs remote; klasifikasi M/A/D; `ignore`.
- `nxzilla push`, `nxzilla pull`, `nxzilla status`, `--dry-run`.
- Deteksi konflik + `--force`.
- **DoD:** mengubah berkas lokal lalu `nxzilla push` memperbarui server; dan
  sebaliknya.

### Fase 5 — Registry proyek & ruang global
- `projects.json`: tambah/hapus/scan/refresh status.
- Menu global (bagian 3): daftar proyek, buka, push/pull dari daftar, kelola
  profil.
- Auto-daftar saat proyek dibuat; `nxzilla scan`.
- **DoD:** dari luar proyek, `nxzilla` menampilkan **semua** proyek Nxzilla dan
  bisa membuka/mengelolanya.

### Fase 6 — TUI dashboard (proyek + global)
- Dashboard gaya NxPort/NxfireDB: koneksi, status, daftar perubahan,
  progress transfer, kontrol tombol — untuk kedua konteks.
- **DoD:** seluruh alur bisa dijalankan tanpa mengetik perintah lagi.

### Fase 7 — Keamanan kredensial & kenyamanan
- Kredensial manager OS / env var; obfuscate minimal.
- Statistik & ringkasan; `watch`; `edit <file>`.
- **DoD:** password tidak tersimpan polos; `nxzilla edit` bekerja.

### Fase 8 — Docs & uji akhir
- `README.md` final, contoh sesi, panduan pemasangan ke PATH (seperti NxPort).
- Uji end-to-end dengan server SSH & FTP uji (Docker/`ssh localhost`).
- **DoD:** semua uji hijau; docs sinkron.

---

## 8. Risiko

| Risiko | Dampak | Mitigasi |
|---|---|---|
| OpenSSH tidak tersedia di Windows lama | Sedang | Deteksi; sarankan install / opsi libssh2 |
| Prompt password/host-key memblokir alur batch | Sedang | Mode batch terkendali; `-o BatchMode` + key |
| Transfer besar memblokir TUI | Sedang | Thread terpisah + progress; batas buffer |
| Konflik dua arah menimpa kerja user | Tinggi | Deteksi konflik, `--dry-run`, snapshot/rollback |
| Registry basi (folder proyek dipindah/dihapus) | Rendah | Validasi path saat buka; tandai `unknown`/`error` |
| Uji status semua proyek lambat (banyak proyek) | Sedang | Lazy/ping selektif; refresh on-demand |
| Perbedaan format listing FTP antar server | Sedang | Parser LIST toleran / `MLSD` bila ada |
| Kredensial bocor | Tinggi | Kredensial manager OS, env var, tak simpan polos |

---

## 9. Lanskap & Pesaing

Inti Nxzilla (transfer/sinkron) sudah banyak diisi; yang lebih tipis adalah
kemasan untuk ceruk Windows + TUI + FTP.

| Tool | Yang sudah dilakukan | Kekurangan vs Nxzilla |
|---|---|---|
| `rsync` | Sinkron inkremental, ignore, arah dua kali | Non-TUI, tak ada profil/proyek, hafal flag |
| `lftp` (`mirror -R`) | Klien FTP/SFTP + mirror | Non-TUI, konfigurasi manual |
| `rclone` (`bisync`, `mount`) | Sync dua arah + mount | Setup & `bisync` rumit |
| `Unison` | Sinkron dua arah paling matang | Non-TUI, setup berat |
| **Mutagen** | Sinkron proyek dev (SSH/Docker) — paling dekat | Daemon, bukan TUI "run-in-cwd" |
| `sshfs` / WinFsp | Remote FS masuk folder (ide `cd`) | Bukan sync+push; latensi |
| VS Code Remote-SSH / Gateway | Edit langsung di remote | Bukan terminal-native |
| WinSCP / FileZilla / Cyberduck | GUI + profil koneksi | GUI, bukan TUI, bukan proyek-di-cd |
| Git deploy (Deployer/Capistrano) | Deploy standar modern | Butuh repo & commit |

**Kesimpulan:** bukan teknologi baru; peluangnya di **pengalaman** — satu
perintah dari terminal, profil + registry proyek, dukungan FTP, dan "beranda"
daftar proyek. Fokus pada ceruk itu; jangan menulis ulang mesin sinkron dari
nol (bungkus `rsync`/`lftp`/`sftp`).

---

## 10. Keputusan Tersisa

1. **Backend koneksi** — subprocess OpenSSH (tanpa dependensi, cepat mulai)
   atau `libssh2` (kontrol penuh, koneksi persisten)? *Usulan: OpenSSH dulu.*
2. **Bahasa/struktur** — single-file `nxzilla.cpp` seperti NxPort, atau
   modular + CMake seperti NxfireDB? *Usulan: single-file dulu.*
3. **Arah sinkron default** — saat `nxzilla` pertama dijalankan: tarik penuh
   (sesuai README), atau tampilkan status dulu lalu user memilih? *Usulan:
   tarik penuh, dengan `--dry-run` untuk melihat dulu.*
4. **Scope FTP** — perlu FTPS (TLS) sejak awal, atau FTP saja dulu?
5. **"Buka proyek" dari ruang global** — masuk mode proyek saja, buka subshell
   di folder proyek, atau cetak path untuk `cd`? (lihat catatan bagian 3)
6. **`nxzilla plus`** — fitur tambahan apa yang dimaksud pada baris
   "nxzilla plus" di README? (perlu diklarifikasi user)

---

## 11. Langkah Berikutnya

**v1.0.0 sudah rilis** (Windows + Linux). Alur inti sudah hidup dan teruji:
`nxzilla add`/`scan` → `pull` (tarik proyek ke cd) → edit lokal → `push`
(kirim balik), dengan diff A/D/M, `ignore`, konflik + `--force`, `--dry-run`,
dan `--checksum`; plus TUI dengan progress.

Sisa untuk v1.1+:

1. **OS keyring** untuk kredensial (Windows Credential Manager / `libsecret`) —
   menggantikan obfuskasi.
2. **Uji transfer SSH nyata** (aktifkan `openssh-server` di WSL) untuk menutup
   Fase 2 sepenuhnya.
3. **`nxzilla watch`** (pantau perubahan remote) dan **`nxzilla edit <file>`**
   (tarik → edit di `$EDITOR` → unggah).
4. **Statistik transfer** (jumlah byte, kecepatan, durasi).
5. **Onboarding folder otomatis**: tawarkan membuat `.nxzilla.json` saat
   `nxzilla` dijalankan di folder non-proyek.
6. **Klarifikasi "nxzilla plus"** (bagian 10 nomor 6).
