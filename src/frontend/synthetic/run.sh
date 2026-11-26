#!/bin/bash

if [ -z "${SCARAB_ROOT}" ]; then 
    echo "set SCARAB_ROOT variable"
    exit 1
fi

SCARAB_SRC="${SCARAB_ROOT}/src"
SCARAB="${SCARAB_SRC}/scarab"

if [ ! -f "${SCARAB}" ]; then 
    echo "build scarab first before running this"
    exit 1
fi

INST_LIMIT="10000000"
FRONTEND="synthetic"
ALL_EXPERIMENTS=(
    --icache
    --mem_fu_limited
    --load_latency_limited
    --cbr_limited
    --btb_limited
    --ibr_limited
    --ilp_limited
)
OFF_FDIP="--fdip_enable 0"
UOP_CACHE_SCALE_UP="--uop_cache_lines 524288"
CBR_SCALE_BTB="--btb_entries 1048576 --btb_assoc 1024 --uop_cache_lines 1048576 --uop_cache_assoc 1024 --icache_size 524288 --icache_assoc  512"
UBR_SCALE_BTB="--btb_entries 128 --uop_cache_lines 1048576 --uop_cache_assoc 1024 --icache_size 524288 --icache_assoc  512"
INST_NOP_SIZE="--nop_size 1 --inst_size 64 --pad_length 1024"
MEM_LEVELS=(
    "mlc_level"
    "llc_level"
    "mem_level"
    "dcache_level"
)
t_nt_ratio=(
 "0.5"
 "0.2"
 "0.8"
)

declare -A btb_entries

btb_entries=(
    [contained]="128"
    [full_sweep]="4096"
)
declare -A ibr_target_strategy

ibr_target_strategy=(
    [uniform_random]=4
    [uniform_sequential]=4
)

ilp_dep_chain=(
    "1"
    "2"
    "4"
)

FUTYPES_1FU="b00100100111111100111001001111110010010011111110011100100111111 b10010000110111100011111001110111001000011011110001111100111011 b00000001000000001000000010000010000000100000000100000000000001 b00000001000000001000000010000010000000100000000100000001000001 b00000000110111100011011001110110000000011011110001101100111011 b01001000000000000111001001111110100100000000000011100100111111 b00000010000000010000000100000010000001000000001000000010000001 b00000010000000010000000100000010000001000000001000000010000001"
FUTYPES_2FU="b00100100111111100111001001111110010010011111110011100100111111 b10010000110111100011111001110111001000011011110001111100111011 b00000001000000001000000010000010000000100000000100000001000001 b00000001000000001000000010000010000000100000000100000001000001 b00000000110111100011011001110110000000011011110001101100111011 b01001000000000000111001001111110100100000000000011100100111111 b00000010000000010000000100000010000001000000001000000010000001 b00000010000000010000000100000010000001000000001000000010000001"
FUTYPES_4FU="b00100100111111100111001001111110010010011111110011100100111111 b10010000110111100011111001110111001000011011110001111100111011 b00000001000000001000000010000010000000100000000100000001000001 b00000001000000001000000010000010000000100000000100000001000001 b00000000110111100011011001110110000000011011110001101101111011 b01001000000000000111001001111110100100000000000011100101111111 b00000010000000010000000100000010000001000000001000000010000001 b00000010000000010000000100000010000001000000001000000010000001"

ARGS=()

for arg in "$@"; do
    if [[ "$arg" == "--all" ]]; then
        ARGS+=("${ALL_EXPERIMENTS[@]}")
    else
        ARGS+=("$arg")
    fi
done

for args in "${ARGS[@]}"; do
    case ${args} in 
        --icache)
            mkdir  icache_limited
            cd icache_limited
            rm -r *
            cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
            echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel icache_limited --inst_limit "${INST_LIMIT}" ${OFF_FDIP}"
            "${SCARAB}" --frontend "${FRONTEND}" --kernel icache_limited --inst_limit "${INST_LIMIT}" ${OFF_FDIP} ${INST_NOP_SIZE}
            cd ..
        ;;

        --mem_fu_limited)
            mkdir  mem_band_1fu
            cd     mem_band_1fu
            rm -r *
            cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
            echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_fu_limited  --inst_limit "${INST_LIMIT}"  ${INST_NOP_SIZE}" 
            "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" 
            cd ..
        ;;

        --load_latency_limited)
            mkdir -p load_latency_limited
            cd load_latency_limited
            for item in "${MEM_LEVELS[@]}" ; do
                mkdir -p "${item}"
                cd "${item}"
                cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel load_latency_limited  --inst_limit "${INST_LIMIT}"  ${INST_NOP_SIZE} --mem_hierachy_level "${item}" " 
                "${SCARAB}" --frontend "${FRONTEND}" --kernel load_latency_limited  --inst_limit "${INST_LIMIT}" --mem_hierachy_level "${item}"  
                cd ..
            done 
            cd ..
        ;;

        --cbr_limited)
            mkdir -p cbr_limited
            cd cbr_limited
            for item in "${t_nt_ratio[@]}"; do
                 mkdir -p "${item}" 
                 cd "${item}"
                 cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                 echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "cbr_limited" --inst_limit "${INST_LIMIT}" ${CBR_SCALE_BTB} --t_nt_ratio "${item}" --direction_sampling_strategy discrete_random" 
                 "${SCARAB}" --frontend "${FRONTEND}" --kernel "cbr_limited" --inst_limit "${INST_LIMIT}" ${CBR_SCALE_BTB} --t_nt_ratio "${item}" --direction_sampling_strategy discrete_random --nop_size 1 --inst_size 1 --target_stride 8 --target_sampling_strategy uniform_random 
                 cd ..
            done 
            cd ..
        ;;
        
        --btb_limited)
            mkdir -p btb_limited 
            cd btb_limited
            for key in "${!btb_entries[@]}"; do
                mkdir -p "${key}" 
                cd "${key}"
                cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                 echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "btb_limited" --inst_limit "${INST_LIMIT}" ${UBR_SCALE_BTB} --nop_size 1 --inst_size 1 --target_stride 8 --target_sampling_strategy uniform_random  --target_pool_size "${btb_entries[${key}]}" "
                 "${SCARAB}" --frontend "${FRONTEND}" --kernel "btb_limited" --inst_limit "${INST_LIMIT}" ${UBR_SCALE_BTB} --nop_size 1 --inst_size 1 --target_stride 6 --target_sampling_strategy uniform_random --target_pool_size "${btb_entries[${key}]}"
                 cd ..
            done 
            cd ..
        ;;


        --ibr_limited)
            mkdir -p ibr_limited
            cd ibr_limited
            for key in "${!ibr_target_strategy[@]}"; do
                mkdir -p "${key}"
                cd "${key}"
                cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "ibr_limited" --inst_limit "1000000" ${CBR_SCALE_BTB} --target_sampling_strategy "${key}" --target_pool_size "${ibr_target_strategy[${key}]}" --nop_size 1 --inst_size 1 --target_stride 6 " 
                "${SCARAB}" --frontend "${FRONTEND}" --kernel "ibr_limited" --inst_limit "1000000" ${CBR_SCALE_BTB}  --target_sampling_strategy "${key}" --target_pool_size "${ibr_target_strategy[${key}]}" --nop_size 1 --inst_size 1 --target_stride 6  
                cd ..
            done
            cd ..
        ;;

        --ilp_limited)
            mkdir -p ilp_limited
            cd ilp_limited
            for item in "${ilp_dep_chain[@]}"; do
                mkdir -p "${item}"
                cd  "${item}"
                cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "ilp_limited" --inst_limit "${INST_LIMIT}" ${UBR_SCALE_BTB} --nop_size 1 --inst_size 1 --dependence_chain_count "${item}" "
                "${SCARAB}" --frontend "${FRONTEND}" --kernel "ilp_limited" --inst_limit "${INST_LIMIT}" ${UBR_SCALE_BTB} --nop_size 1 --inst_size 1 --dependence_chain_count "${item}"
                 cd ..
            done
            cd ..
        ;;

        *)
            echo "unknown option"
            exit 1
        ;;
    esac
done