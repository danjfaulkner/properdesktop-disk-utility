# Disk Utility

The disk utility of Proper Desktop. It lists the internal and external
disks and the attached disk images with their partitions and volumes, and
shows each one's capacity, usage, connection and S.M.A.R.T. health.

It mounts, unmounts and ejects; erases a disk or volume as ext4, exFAT,
FAT32, NTFS, Btrfs or XFS, optionally encrypted, with a GUID or MBR
partition map; partitions a disk on a pie chart, adding, deleting and
resizing partitions in one plan; checks and repairs filesystems; unlocks,
locks and renames volumes; attaches, creates and makes images of disks and
volumes; and restores a volume or image onto another.

The disks come from UDisks2, whose own authorization asks for a password
where one is needed. The volumes and disk of the running system are never
erased, partitioned, repaired or unmounted.

<img src="screenshots/disk-utility.png" alt="Disk Utility" width="945">

## Dependencies

Disk Utility uses properdesktop-toolkit, and UDisks2 at run time; GoogleTest
for the tests. Each format needs its tools, which are optional: a format
without them is offered disabled. e2fsprogs (ext4), exfatprogs (exFAT),
dosfstools (FAT32), ntfs-3g (NTFS), btrfs-progs (Btrfs), xfsprogs (XFS).

```sh
sudo apt install cmake udisks2 libgtest-dev
sudo apt install e2fsprogs exfatprogs dosfstools ntfs-3g btrfs-progs xfsprogs
```

## Build and install

```sh
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
sudo make install
```

## Tests

```sh
cmake -DBUILD_TESTING=ON ..
make
dbus-run-session -- ctest --output-on-failure
```

## License

BSD-3-Clause; see [LICENSE](LICENSE).
