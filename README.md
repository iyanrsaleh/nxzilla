# Nxzilla

**Jembatan terminal ke server remote.**

Buka terminal di folder proyek, jalankan `nxzilla`, dan berkas proyek remote
masuk ke folder kerja (cd) tersebut. Perubahan dikirim balik ke server lewat
perintah. Satu alur, dari terminal.

**Versi terbaru: v1.0.0** — unduh biner siap pakai di
[halaman Releases](https://github.com/iyanrsaleh/nxzilla/releases):
`nxzilla-windows-x64.exe` (Windows) dan `nxzilla-linux-x64` (Linux).

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

## Latar belakang

Nxzilla lahir dari kesulitan kerja remote: dulu user harus "mendarat mandiri" —
pindah dari FileZilla FTP client ke folder Explorer secara manual. Nxzilla
mengangkat alur itu menjadi satu perintah terminal dengan tampilan TUI, serupa
[`NxPort`](../NxPort) dan [`NxfireDB`](../../Database/NxfireDB).

Nxzilla **bukan** FileZilla. Tidak ada dua panel (lokal vs remote), tidak ada
drag-and-drop. Referensi manual yang ingin digantikan: folder seperti
`D:\info@nxdom.org\ssh` — dulu user menyusun sendiri folder bernama
`<user>@<host>\<protokol>` lalu memindahkan berkas ke sana.

## Alur utama

1. User buka terminal di folder proyek.
2. Jalankan `nxzilla` → masukkan konfigurasi `ssh` atau `ftp` (host, user,
   password, path remote).
3. Berkas proyek remote **ditarik masuk ke folder kerja saat ini**. Struktur
   folder remote dipertahankan.
4. User bekerja seperti biasa dengan berkas lokal (edit di editor favorit).
5. Perubahan dikirim balik ke server dengan `nxzilla push`, atau menarik
   perubahan server dengan `nxzilla pull`.

```powershell
PS D:\Website> nxzilla
```

## Dua lingkungan

| Konteks | Kapan | Yang terjadi |
| :--- | :--- | :--- |
| **Lingkungan proyek** | `nxzilla` dijalankan di dalam folder proyek (ada `.nxzilla.json`) | Sinkronisasi proyek itu (pull/push/status) |
| **Lingkungan global** | `nxzilla` dijalankan di luar proyek | Menu **daftar semua proyek** yang memakai Nxzilla, untuk dibuka & dikelola |

Lingkungan global adalah "beranda" Nxzilla: kamu bisa melihat **proyek apa saja
yang memakai Nxzilla**, dan membuka salah satunya dari situ.

## Status saat ini

Fitur yang sudah jalan (v1.0.0):

- **Build hijau di Windows (MSVC) dan Linux (g++)** — satu basis kode.
- **Backend koneksi**: FTP/FTPS (via `curl`) dan SSH/SFTP (via OpenSSH
  `ssh`/`sftp`).
- **Sinkron cerdas**: banding lokal vs remote pakai **ukuran + mtime**;
  klasifikasi `A` (hanya lokal), `D` (hanya remote), `M` (berubah); hormati
  `ignore`.
- **`pull` / `push` / `status`** hanya mentransfer berkas yang berubah.
- **Deteksi konflik**: berkas yang berubah di dua sisi ditandai `CONFLICT` dan
  dilewati; timpa dengan `--force`.
- **`--checksum`**: banding pakai SHA-1 (akurat walau ukuran & mtime sama).
- **`--dry-run`**: tampilkan rencana transfer tanpa mengubah apa pun.
- **TUI** keyboard-driven bergaya NxPort: tombol push/pull/status, **progress
  per berkas**, editor profil & proyek.
- **Registry proyek global** + profil koneksi (`projects.json`,
  `connections.json`); perintah `add`, `scan`, `use`, `ignore`, `remove`.
- **Onboarding**: menjalankan `nxzilla` di folder non-proyek (yang belum
  terdaftar) langsung menawarkan *"Register it and pull the remote project into
  this folder?"* → form setup terbuka dengan folder sudah terisi.
- **`nxzilla edit <berkas>`**: tarik berkas, buka di `$EDITOR`, unggah balik
  otomatis saat editor ditutup.
- **`nxzilla watch [detik]`**: pantau perubahan remote berkala (mode `--once`
  untuk script/CI).
- **Cache metadata** remote (`.nxzilla-cache.json`) agar `status` berulang cepat;
  `--refresh` memaksa pemindaian ulang. `push`/`pull` selalu memindai ulang.
- **Kredensial** tidak disimpan polos (obfuskasi); `NXZILLA_PASSWORD` untuk
  non-interaktif.
- Fallback teks non-interaktif, `--version`, `--help`. Semua teks UI Inggris.

Belum ada (rencana): OS keyring (kredensial), `watch`/`edit <file>`, statistik
kecepatan, onboarding folder otomatis. Lihat `RENCANA.md`.

## Perintah

```sh
nxzilla                      # dalam proyek: sinkron proyek; di luar: ruang global
nxzilla projects             # paksa buka ruang global (daftar proyek)
nxzilla add                  # daftarkan folder kini sebagai proyek (+ profil)
nxzilla remove <nama>        # hapus dari registry (folder tidak dihapus)
nxzilla scan [folder]        # cari folder ber-.nxzilla.json lalu daftarkan
nxzilla pull                 # tarik perubahan dari server ke folder kerja
nxzilla push                 # kirim perubahan folder kerja ke server
nxzilla status               # tampilkan status sinkronisasi (tanpa mengubah)
nxzilla status --refresh     # paksa pemindaian ulang (abaikan cache)
nxzilla ls [path]            # daftar isi folder remote (tanpa menarik berkas)
nxzilla use <nama-profil>    # set profil untuk folder kerja saat ini
nxzilla ignore <pola>        # tambah pola ignore ke .nxzilla.json
nxzilla edit <berkas>        # tarik berkas, buka di editor, unggah balik
nxzilla watch [detik]        # pantau perubahan remote berkala (--once sekali)
nxzilla push --dry-run       # simulasi, tampilkan berkas yang akan ditransfer
nxzilla push --force         # timpa berkas konflik
nxzilla push --checksum      # banding pakai SHA-1
nxzilla --help               # bantuan
nxzilla --version            # versi
```

## TUI

Kontrol ruang global:

| Tombol | Aksi |
| :--- | :--- |
| `↑`/`↓` | Pilih proyek |
| `Enter` | Buka proyek |
| `p` / `l` | Push / pull proyek terpilih tanpa masuk ke foldernya |
| `a` | Tambah proyek |
| `e` | Edit proyek terpilih |
| `d` | Hapus dari registry (folder **tidak** dihapus, dengan konfirmasi) |
| `c` | Settings: kelola profil koneksi |
| `r` | Refresh daftar |
| `q` / `Esc` | Keluar |

Kontrol TUI proyek: `p` push, `l` pull, `s` status, `g` ruang global,
`c` settings, `q` keluar.

## Konfigurasi

Profil koneksi global:

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
      "password": "••••••",
      "remotePath": "/var/www/nxdom"
    }
  ]
}
```

Konfigurasi per-proyek `.nxzilla.json` di folder kerja (menandai folder sebagai
proyek Nxzilla):

```jsonc
{
  "profile": "nxdom-ssh",        // rujuk nama di connections.json
  "remotePath": "/var/www/nxdom",// boleh menimpa remotePath profil
  "lastSync": "2026-10-13T10:20:00Z",
  "ignore": [".git/", "node_modules/", "*.log", ".env"]
}
```

Registry proyek global:

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
    }
  ]
}
```

## Build

Punya biner siap pakai? Lihat [Releases](https://github.com/iyanrsaleh/nxzilla/releases).
Dari sumber:

Windows (MSVC via vswhere):

```powershell
.\build.bat
```

Linux (g++):

```sh
./build.sh
```

Hasil: `nxzilla.exe` (Windows) atau `nxzilla` (Linux). Tambahkan folder ini ke
`PATH` agar bisa dipanggil dari mana saja.

FTP/FTPS memerlukan `curl` di PATH; SSH/SFTP memerlukan klien OpenSSH (`ssh`,
`sftp`) — keduanya tersedia bawaan di Windows 10/11 dan mayoritas distro Linux.

## Struktur repo

```
Nxzilla/
  nxzilla.cpp               inti: TUI + registry + konfigurasi (single file)
  build.bat                 build Windows (MSVC)
  build.sh                  build Linux (g++)
  connections.example.json  contoh profil koneksi
  .nxzilla.json.example     contoh konfigurasi proyek
  projects.example.json     contoh registry proyek global
  README.md                 ringkasan proyek (dokumen ini)
  RENCANA.md                rencana pengembangan lengkap
  LICENSE                   MIT
```

## Dokumen lain

- [`RENCANA.md`](RENCANA.md) — rencana lengkap: konfigurasi, komponen teknis,
  roadmap per fase, risiko, lanskap pesaing, keputusan tersisa.
- `LICENSE` — MIT.
</content>
