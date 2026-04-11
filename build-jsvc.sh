#!/bin/bash
# Build jsvc binaries using CentOS 6 Docker container
# Produces binaries compatible with GNU/Linux 2.6.18 and GLIBC 2.2.5
#
# Usage:
#   ./build-jsvc.sh           # build both 64-bit and 32-bit
#   ./build-jsvc.sh 64        # build 64-bit only
#   ./build-jsvc.sh 32        # build 32-bit only

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/src/native/unix"

CENTOS6_IMAGE="centos:6"
PLATFORM="linux/amd64"

COMMON_SETUP='
  sed -i "s|mirrorlist=|#mirrorlist=|g" /etc/yum.repos.d/CentOS-Base.repo
  sed -i "s|#baseurl=http://mirror.centos.org/centos/[^/]*/|baseurl=http://vault.centos.org/centos/6.10/|g" /etc/yum.repos.d/CentOS-Base.repo
'

build_64() {
  echo "==> Building 64-bit jsvc..."
  docker run --rm --platform "$PLATFORM" \
    -v "$SCRIPT_DIR:/src" \
    "$CENTOS6_IMAGE" bash -c "
      set -e
      $COMMON_SETUP
      yum install -y autoconf automake make gcc java-1.8.0-openjdk-devel 2>&1 | grep -E 'Complete|Error'
      JAVA_HOME=\$(dirname \$(dirname \$(readlink -f \$(which javac))))
      AMDIR=\$(find /usr/share/automake* -maxdepth 0 | head -1)
      cd /src/src/native/unix
      make clean 2>/dev/null || true
      sh support/buildconf.sh
      cp \$AMDIR/install-sh \$AMDIR/config.sub \$AMDIR/config.guess support/
      ./configure --with-java=\$JAVA_HOME
      make
      echo '==> 64-bit build done'
      file jsvc
      objdump -T jsvc | grep GLIBC | sed 's/.*GLIBC_/GLIBC_/' | awk '{print \$1}' | sort -u
    "
  echo "==> Output: $OUTPUT_DIR/jsvc"
}

build_32() {
  echo "==> Building 32-bit jsvc..."
  docker run --rm --platform "$PLATFORM" \
    -v "$SCRIPT_DIR:/src" \
    "$CENTOS6_IMAGE" bash -c "
      set -e
      $COMMON_SETUP
      yum install -y autoconf automake make gcc glibc-devel glibc-devel.i686 libgcc.i686 java-1.8.0-openjdk-devel 2>&1 | grep -E 'Complete|Error'
      JAVA_HOME=\$(dirname \$(dirname \$(readlink -f \$(which javac))))
      AMDIR=\$(find /usr/share/automake* -maxdepth 0 | head -1)
      cd /src/src/native/unix
      make clean 2>/dev/null || true
      sh support/buildconf.sh
      cp \$AMDIR/install-sh \$AMDIR/config.sub \$AMDIR/config.guess support/
      CC='gcc -m32' ./configure --with-java=\$JAVA_HOME --build=i686-pc-linux-gnu
      make
      cp jsvc jsvc32
      echo '==> 32-bit build done'
      file jsvc32
      objdump -T jsvc32 | grep GLIBC | sed 's/.*GLIBC_/GLIBC_/' | awk '{print \$1}' | sort -u
    "
  echo "==> Output: $OUTPUT_DIR/jsvc32"
}

MODE="${1:-both}"

case "$MODE" in
  64)   build_64 ;;
  32)   build_32 ;;
  both) build_64; build_32 ;;
  *)    echo "Usage: $0 [64|32|both]"; exit 1 ;;
esac

echo ""
echo "==> All done. Binaries:"
ls -lh "$OUTPUT_DIR/jsvc" "$OUTPUT_DIR/jsvc32" 2>/dev/null || true
