The following are sample commands to illustrate the process of signing the Ghaf installer ISO for Secure Boot.

The idea is to progressively unpack the layers of the CD image until we reach the raw NixOS disk image. At this point we
can sign it with the `sign_disk_image.sh` script and recreate the installer by doing the steps in reverse.

The commands have been gathered from manual experimentation and so it is possible that this cannot be run as a complete
script as is.

```shell
# This contains a patch to output the make-iso-image pathlist in the derivation
nix build .#lenovo-x1-carbon-gen11-debug-installer

mkdir ../installer
cp result/iso/ghaf.iso ../installer/
cp result/pathlist ../installer/
cd ../installer

# Extract the installer nix store
mkdir mnt
sudo mount -t iso9660 ghaf.iso mnt
cp mnt/nix-store.squashfs .
sudo umount mnt

# Extract the Ghaf compressed disk image from the nix store
unsquashfs nix-store.squashfs -extract-file 'wacbgdwwcdmxmd95w234cn9rnr68vria-ghaf-host-disko-images/disk1.raw.zst'

# Decompress the image and sign binaries
zstd -d squashfs-root/wacbgdwwcdmxmd95w234cn9rnr68vria-ghaf-host-disko-images/disk1.raw.zst -o disk1.img
export VAULT_TOKEN='......'
../ghaf-sb/packages/pkgs-by-name/sign-uki-secureboot/sign_disk_image.sh disk1.img
zstd --compress disk1.img -o disk1.raw.zst --rm

# Packing up...
# Recreate a squashfs with the signed image
mkdir rootfs
sudo mount -t squashfs nix-store.squashfs rootfs
mkdir overlay upper workdir
sudo mount -t overlay -o lowerdir=rootfs,upperdir=upper,workdir=workdir overlay overlay
sudo cp disk1.raw.zst overlay/wacbgdwwcdmxmd95w234cn9rnr68vria-ghaf-host-disko-images/disk1.raw.zst
sudo mksquashfs overlay/* nix-store-mod.squashfs -no-hardlinks -keep-as-directory -all-root -b 1048576 -comp zstd -Xcompression-level 3 -root-mode 0755
sudo umount overlay
sudo umount rootfs
mv nix-store-mod.squashfs nix-store.squashfs

# Make the iso
xorriso -boot_image any gpt_disk_guid=5c0af4a4af1651f18ca7257154097569 -volume_date all_file_dates '=315532800' -as mkisofs -iso-level 3 -volid nixos-minimal-25.05-x86_64 -appid nixos -publisher nixos -graft-points -full-iso9660-filenames -joliet -eltorito-boot isolinux/isolinux.bin -eltorito-catalog .boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table --sort-weight 1 /isolinux -isohybrid-mbr /nix/store/cv25ivm3m22jnl6pdgsw37xz07hzzaca-syslinux-unstable-2019-02-07/share/syslinux/isohdpfx.bin -eltorito-alt-boot -e boot/efi.img -no-emul-boot -isohybrid-gpt-basdat -r -path-list pathlist --sort-weight 0 / -output ghaf-signed.iso
```