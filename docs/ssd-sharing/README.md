# Raspberry Pi SSD sharing with Samba and Tailscale

Use the USB SSD as a network drive in Mac Finder, both at home and remotely.
The SSD keeps its existing exFAT filesystem and can still connect directly to a Mac.

## Your setup

| Setting | Value |
| --- | --- |
| Pi username | `jasonyu0705` |
| Pi LAN address, last checked | `10.0.0.248` |
| Pi Tailscale address | `100.96.90.20` |
| SSD label | `Jasons SSD` |
| SSD filesystem and UUID | `exfat`, `42FB-9470` |
| Account UID and GID | `1000`, `1000` |
| Mount folder | `/mnt/jasons-ssd` |
| Samba share name | `SSD` |

Run Linux commands in the Pi's SSH terminal. Follow the Finder steps on your Mac.
If a command fails, resolve that error before moving on.

## 1. Connect and identify the SSD

Connect the SSD to the Pi through USB. If needed, connect from your Mac:

```bash
ssh jasonyu0705@10.0.0.248
```

Check the drive and your account:

```bash
lsblk -o NAME,SIZE,FSTYPE,LABEL,UUID,MOUNTPOINTS,MODEL
id
tailscale ip -4
```

Confirm the SSD UUID is `42FB-9470`, its filesystem is `exfat`, and your UID/GID
are both `1000`. The Pi's SD card contains `/` and `/boot/firmware`; it is separate
from the USB SSD. These instructions use the SSD's UUID to identify it.

## 2. Mount the SSD

Check whether it is already mounted:

```bash
findmnt /mnt/jasons-ssd
```

If it shows the correct SSD mounted as `exfat` with `rw`, continue to step 3.
Otherwise, mount it:

```bash
sudo mkdir -p /mnt/jasons-ssd
sudo mount -t exfat -o "uid=$(id -u),gid=$(id -g),fmask=0177,dmask=0077" /dev/disk/by-uuid/42FB-9470 /mnt/jasons-ssd
findmnt /mnt/jasons-ssd
```

These options give your Pi account access to the files. They do not reformat the
drive. [exFAT ownership and permissions](https://manpages.debian.org/trixie/exfat-fuse/mount.exfat-fuse.8.en.html)

## 3. Mount automatically after reboot

Back up and edit the mount configuration:

```bash
sudo cp /etc/fstab /etc/fstab.before-ssd
sudo nano /etc/fstab
```

Keep the existing contents. Add the following as ONE new line at the bottom.
If an entry for this UUID already exists, edit that entry instead of adding a duplicate.

```text
UUID=42FB-9470 /mnt/jasons-ssd exfat defaults,nofail,uid=1000,gid=1000,fmask=0177,dmask=0077,x-systemd.device-timeout=5s 0 0
```

In nano, press Ctrl+O, Enter, then Ctrl+X to save and exit.

Check the configuration:

```bash
sudo findmnt --verify
```

Resolve any reported errors before continuing. Then apply it:

```bash
sudo systemctl daemon-reload
sudo mount -a
findmnt /mnt/jasons-ssd
```

The drive mounts at boot when connected. `nofail` allows boot without the SSD;
the device wait is limited to five seconds. Plugging it back in while the Pi is
already running requires the manual mount command in step 7.
[Mount options](https://www.freedesktop.org/software/systemd/man/latest/systemd.mount.html)

## 4. Install Samba and set your sharing password

```bash
sudo apt update
sudo apt install samba
sudo smbpasswd -a jasonyu0705
```

Choose a Samba password when prompted. This is the password you will use in
Finder, and it can differ from your Pi login password. Password entry is hidden.

## 5. Create and start the SSD share

Back up and open Samba's configuration:

```bash
sudo cp /etc/samba/smb.conf /etc/samba/smb.conf.before-ssd
sudo nano /etc/samba/smb.conf
```

Add this at the bottom. If an `[SSD]` section already exists, update it instead
of creating a second one.

```ini
[SSD]
    comment = Jasons SSD
    path = /mnt/jasons-ssd
    browseable = yes
    read only = no
    guest ok = no
    valid users = jasonyu0705
    preexec = /usr/bin/mountpoint -q /mnt/jasons-ssd
    preexec close = yes
```

The share requires your Samba login. The last two settings reject a connection
when the SSD is not mounted. Save with Ctrl+O, Enter, then Ctrl+X.
[Samba share settings](https://www.samba.org/samba/docs/current/man-html/smb.conf.5.html)

Validate the configuration:

```bash
sudo testparm -s
```

If it loads successfully, enable the service at boot and restart it:

```bash
sudo systemctl enable smbd
sudo systemctl restart smbd
systemctl is-active smbd
```

The last command should print `active`.

## 6. Connect in Mac Finder

1. Install/open Tailscale on your Mac and connect it to the same Tailscale network
   as the Pi. It can run in the background; its window does not need to stay open.
2. Open Finder and press Command+K (Go > Connect to Server).
3. Enter `smb://100.96.90.20/SSD` and click Connect.
4. Select Registered User.
5. Enter `jasonyu0705` and the Samba password from step 4.
6. Optionally save the password in your Mac's keychain and add the server using
   the plus button in Connect to Server for convenient reconnection.
7. Create a small test folder and copy a file into it to verify read/write access.

The SSD appears as a network drive in Finder. Tailscale must be connected on
both devices when using the Tailscale address. At home, you can instead use
`smb://10.0.0.248/SSD` without Tailscale, provided the LAN address is unchanged.
[Tailscale Finder connection guide](https://tailscale.com/docs/use-cases/personal-or-at-home-use/access-nas-media-file-servers?tab=macos)

To verify remote access, connect your Mac to a phone hotspot with Tailscale on,
then reconnect using `smb://100.96.90.20/SSD`. The Pi stays on your home network.

## 7. Move the SSD between the Pi and Mac

Before disconnecting it from the Pi:

1. Finish transfers, close files, and eject the network drive in Finder on any
   connected Macs. Close other computers' connections too.
2. In the Pi terminal, leave the SSD directory and unmount it:

```bash
cd ~
sudo umount /mnt/jasons-ssd
```

Only unplug after the command succeeds. If it reports `target is busy`, close
remaining files/connections and retry; leave the drive connected until it succeeds.
You can then plug it directly into the Mac and use it normally.

Before returning it to the Pi, eject the physical SSD in Finder, then unplug it.
Connect it to the running Pi and run:

```bash
sudo mount /mnt/jasons-ssd
findmnt /mnt/jasons-ssd
```

Reconnect the network drive in Finder. If the SSD is attached when the Pi boots,
the mount happens automatically. File sharing is unavailable while the SSD is absent.

## Troubleshooting

- **Finder cannot connect:** check that the SSD is mounted, `smbd` is active,
  and Tailscale is connected on both devices when using the Tailscale address.
- **Login rejected:** use the Samba password. Reset it on the Pi with
  `sudo smbpasswd jasonyu0705` if needed.
- **Permission denied:** inspect `findmnt /mnt/jasons-ssd` and confirm it is
  read/write with `uid=1000,gid=1000`. Use the exFAT mount options above rather
  than changing ownership recursively.
- **Need service logs:** run
  `sudo journalctl -u smbd -n 40 --no-pager`.

The Pi and mounted SSD must stay powered on for network access. Local transfers
depend on your home network; remote transfers also depend on your home upload speed.

## Read or edit this guide in the terminal

```bash
less README.md
nano README.md
```

Press `q` to leave less. In nano, save with Ctrl+O, Enter, then exit with Ctrl+X.
