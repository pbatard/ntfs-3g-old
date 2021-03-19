#
# NOTICE: This script is *NOT* designed to be used in a standalone fashion.
# It is designed to be used as part of an Appveyor build, and only works if the
# preliminaries from the appveyor.yml 'install' section have been run first.
#
export GCC5_ARM_PREFIX=arm-linux-gnueabi-
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
export WORKSPACE=$PWD
export PACKAGES_PATH=$WORKSPACE:$WORKSPACE/edk2
source edk2/edksetup.sh || exit 1
build -a X64 -b RELEASE -t GCC5 -p uefi-driver.dsc -D FORCE_READONLY -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/X64/ntfs.efi $WORKSPACE/ntfs_x64_ro.efi || exit 1
build -a X64 -b RELEASE -t GCC5 -p uefi-driver.dsc -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/X64/ntfs.efi $WORKSPACE/ntfs_x64.efi || exit 1
build -a IA32 -b RELEASE -t GCC5 -p uefi-driver.dsc -D FORCE_READONLY -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/IA32/ntfs.efi $WORKSPACE/ntfs_ia32_ro.efi || exit 1
build -a IA32 -b RELEASE -t GCC5 -p uefi-driver.dsc -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/IA32/ntfs.efi $WORKSPACE/ntfs_ia32.efi || exit 1
build -a AARCH64 -b RELEASE -t GCC5 -p uefi-driver.dsc -D FORCE_READONLY -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/AARCH64/ntfs.efi $WORKSPACE/ntfs_aa64_ro.efi || exit 1
build -a AARCH64 -b RELEASE -t GCC5 -p uefi-driver.dsc -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/AARCH64/ntfs.efi $WORKSPACE/ntfs_aa64.efi || exit 1
build -a ARM -b RELEASE -t GCC5 -p uefi-driver.dsc -D FORCE_READONLY -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/ARM/ntfs.efi $WORKSPACE/ntfs_arm_ro.efi || exit 1
build -a ARM -b RELEASE -t GCC5 -p uefi-driver.dsc -D COMMIT_INFO=${APPVEYOR_REPO_COMMIT:0:8} -D DRIVER_VERSION=$APPVEYOR_REPO_TAG_NAME || exit 1
mv $WORKSPACE/Build/RELEASE_GCC5/ARM/ntfs.efi $WORKSPACE/ntfs_arm.efi || exit 1
