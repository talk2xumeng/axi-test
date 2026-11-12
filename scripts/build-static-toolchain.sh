#!/usr/bin/env bash
set -euo pipefail

# 固定的、可复现的 axiperf 静态构建工具链。
DPDK_VERSION=24.11.4
DPDK_COMMIT=49c5a4aa52658c80359b63e712c80a4dac135259
RDMA_VERSION=50.0
RDMA_COMMIT=bc6b4bc134532e952fe7f8efc251e1f89b912098

PREFIX=${PREFIX:-/opt/axi-toolchain}
WORK=${WORK:-$PWD/build/toolchain}
JOBS=${JOBS:-$(nproc)}
RDMA_PREFIX=$PREFIX/rdma-core-$RDMA_VERSION
DPDK_PREFIX=$PREFIX/dpdk-$DPDK_VERSION

need() {
	command -v "$1" >/dev/null || { echo "缺少构建工具：$1" >&2; exit 1; }
}
need git
need cmake
need ninja
need python3
if command -v meson >/dev/null; then
	MESON=(meson)
elif python3 -c 'import mesonbuild' 2>/dev/null; then
	MESON=(python3 -m mesonbuild.mesonmain)
else
	echo "缺少构建工具：meson" >&2
	exit 1
fi

mkdir -p "$WORK" "$PREFIX"

checkout() {
	local url=$1 commit=$2 dir=$3
	if [ ! -d "$dir/.git" ]; then
		git clone --filter=blob:none --no-checkout "$url" "$dir"
	fi
	if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]; then
		git -C "$dir" fetch --depth 1 origin "$commit"
		git -C "$dir" checkout --detach "$commit"
	fi
	test "$(git -C "$dir" rev-parse HEAD)" = "$commit"
}

checkout https://github.com/linux-rdma/rdma-core.git "$RDMA_COMMIT" "$WORK/rdma-core"
cmake -S "$WORK/rdma-core" -B "$WORK/rdma-build" -GNinja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DCMAKE_INSTALL_PREFIX="$RDMA_PREFIX" \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DENABLE_STATIC=1 \
	-DNO_PYVERBS=1 \
	-DNO_MAN_PAGES=1
cmake --build "$WORK/rdma-build" -j "$JOBS"
cmake --install "$WORK/rdma-build"
# rdma-core 的静态 pc 默认列出所有 provider。axiperf 只使用 mlx5，裁掉其余
# provider，DPDK 的 options-ibverbs-static.sh 会据此定义 RDMA_STATIC_PROVIDERS。
sed -i 's/^Libs.private:.*/Libs.private: -lmlx5 -libverbs -lpthread/' \
	"$RDMA_PREFIX/lib/pkgconfig/libibverbs.pc"

checkout https://github.com/DPDK/dpdk.git "$DPDK_COMMIT" "$WORK/dpdk"
export PKG_CONFIG_PATH="$RDMA_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
"${MESON[@]}" setup "$WORK/dpdk-build" "$WORK/dpdk" --wipe \
	--prefix "$DPDK_PREFIX" \
	--libdir lib/x86_64-linux-gnu \
	-Dbuildtype=release \
	-Ddefault_library=static \
	-Dibverbs_link=static \
	-Denable_drivers=bus/auxiliary,common/mlx5,net/mlx5
"${MESON[@]}" compile -C "$WORK/dpdk-build" -j "$JOBS"
"${MESON[@]}" install -C "$WORK/dpdk-build"

printf '\n工具链已安装。构建 axiperf：\n'
printf 'make static DPDK_ROOT=%q RDMA_ROOT=%q\n' "$DPDK_PREFIX" "$RDMA_PREFIX"
