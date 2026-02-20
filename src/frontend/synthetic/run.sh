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
kernels=(
            "icache_limited"  
            "mem_bandwidth_limited_1FU"
            "mem_bandwidth_limited_2FU"
            "mem_bandwidth_limited_4FU"                         
            "dcache_limited"                 
            "mlc_limited"                    
            "llc_limited"                                      
            "ilp_limited_1_dep_chain"        
            "ilp_limited_2_dep_chain"        
            "ilp_limited_4_dep_chain"        
            "cbr_limited_20t"                
            "cbr_limited_50t"                
            "cbr_limited_80t"                
            "btb_limited_full_capacity_sweep"
            "btb_limited_assoc_sweep"        
            "ibr_limited_RR_4Tgts"            
            "ibr_limited_random_4Tgts"       
            "ibr_limited_random_2Tgts"    
            "mem_limited"  
        ) 
OFF_FDIP="--fdip_enable 0"
UOP_CACHE_SCALE_UP="--uop_cache_lines 524288"
CBR_SCALE_BTB="--btb_entries 16384 --btb_assoc 16"
UBR_SCALE_BTB="--btb_entries 128"

FUTYPES_1FU="b00100100111111100111001001111110010010011111110011100100111111 \
             b10010000110111100011111001110111001000011011110001111100111011 \
             b00000001000000001000000010000010000000100000000100000000000001 \
             b00000001000000001000000010000010000000100000000100000001000001 \
             b00000000110111100011011001110110000000011011110001101100111011 \
             b01001000000000000111001001111110100100000000000011100100111111 \
             b00000010000000010000000100000010000001000000001000000010000001 \
             b00000010000000010000000100000010000001000000001000000010000001"

FUTYPES_2FU="b00100100111111100111001001111110010010011111110011100100111111 \
             b10010000110111100011111001110111001000011011110001111100111011 \
             b00000001000000001000000010000010000000100000000100000001000001 \
             b00000001000000001000000010000010000000100000000100000001000001 \
             b00000000110111100011011001110110000000011011110001101100111011 \
             b01001000000000000111001001111110100100000000000011100100111111 \
             b00000010000000010000000100000010000001000000001000000010000001 \
             b00000010000000010000000100000010000001000000001000000010000001"

FUTYPES_4FU="b00100100111111100111001001111110010010011111110011100100111111 \
             b10010000110111100011111001110111001000011011110001111100111011 \
             b00000001000000001000000010000010000000100000000100000001000001 \
             b00000001000000001000000010000010000000100000000100000001000001 \
             b00000000110111100011011001110110000000011011110001101101111011 \
             b01001000000000000111001001111110100100000000000011100101111111 \
             b00000010000000010000000100000010000001000000001000000010000001 \
             b00000010000000010000000100000010000001000000001000000010000001"             


for args in "${@}"; do
    case ${args} in 
        --icache)
            mkdir  icache_limited
            cd icache_limited
            rm -r *
            cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
            echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel icache_limited --inst_limit "${INST_LIMIT}" ${OFF_FDIP}"
            "${SCARAB}" --frontend "${FRONTEND}" --kernel icache_limited --inst_limit "${INST_LIMIT}" ${OFF_FDIP}
            cd ..
        ;;

        --mem_bandwidth_limited_1FU)
            mkdir  mem_band_1fu
            cd     mem_band_1fu
            rm -r *
            cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
            echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_1FU}"" 
            "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_1FU}"
            cd ..
        ;;

        --mem_bandwidth_limited_2FU)
            mkdir  mem_band_2fu
            cd     mem_band_2fu
            rm -r *
            cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
            echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_2FU}""
            "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_2FU}"
            cd ..
        ;;

        --mem_bandwidth_limited_4FU)
            mkdir  mem_band_4fu
            cd     mem_band_4fu
            rm -r *
            cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
            echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_4FU}" --dcache_read_ports 6"
            "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_4FU}" --dcache_read_ports 6
            cd ..
        ;;

        --all) 
            for item in "${kernels[@]}" ; do
                mkdir -p "./${item}"
                cd "./${item}/"
                rm -r *
                cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                # "${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}"
                case "${item}" in 
                    icache_limited)
                        echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}" ${OFF_FDIP}"
                        "${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}" ${OFF_FDIP}
                    ;;

                    cbr_limited_20t|cbr_limited_50t|cbr_limited_80t)
                        echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}" ${CBR_SCALE_BTB}"
                        "${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}" ${CBR_SCALE_BTB}
                    ;;

                    btb_limited_full_capacity_sweep|btb_limited_assoc_sweep)
                        echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}" ${UBR_SCALE_BTB}"
                        "${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}" ${UBR_SCALE_BTB}
                    ;;

                    mem_bandwidth_limited_1FU)
                        mkdir  mem_band_1fu
                        cd     mem_band_1fu
                        rm -r *
                        cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                         echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_1FU}" --dcache_read_ports 6"
                         "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_1FU}" --dcache_read_ports 6
                        cd ..
                    ;;

                    mem_bandwidth_limited_2FU)
                        mkdir  mem_band_2fu
                        cd     mem_band_2fu
                        rm -r *
                        cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                        echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_2FU}" --dcache_read_ports 6"
                        "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_2FU}" --dcache_read_ports 6
                        cd ..
                    ;;

                    mem_bandwidth_limited_4FU)
                        mkdir  mem_band_4fu
                        cd     mem_band_4fu
                        rm -r *
                        cp "${SCARAB_SRC}/PARAMS.sunny_cove" PARAMS.in
                        echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_4FU}" --dcache_read_ports 6" 
                        "${SCARAB}" --frontend "${FRONTEND}" --kernel mem_bandwidth_limited  --inst_limit "${INST_LIMIT}" --fu_types "${FUTYPES_4FU}" --dcache_read_ports 6
                        cd ..
                    ;;

                    *)
                       echo ""${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}""
                       "${SCARAB}" --frontend "${FRONTEND}" --kernel "${item}" --inst_limit "${INST_LIMIT}"
                    ;;
                esac
                cd ..
            done
        ;;

        *)
            echo "unknown option"
            exit 1
        ;;
    esac
done