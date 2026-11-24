SCRIPT=$(readlink -f "$0")
SCRIPTDIR=$(dirname "$SCRIPT")

DRIO_BUILD_DIR=${SCRIPTDIR}/../../src/build/opt/deps/dynamorio
if [ ! -x ${DRIO_BUILD_DIR}/clients/bin64/drraw2trace ]; then
    echo "drraw2trace not built; run: make -C ${DRIO_BUILD_DIR}" >&2
    exit 1
fi

for dir in */; do
    echo "$dir"
    cd $dir
    mkdir -p bin
    cp raw/modules.log bin/modules.log
    python2 $SCRIPTDIR/portabilize_trace.py .
    cp bin/modules.log raw/modules.log
    ${DRIO_BUILD_DIR}/clients/bin64/drraw2trace -indir ./raw/ &
    cd -
done
