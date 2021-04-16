#!/bin/bash
python3 stacked-bar.py three-cs &
python3 stacked-bar.py temporal-stream &
python3 stacked-bar.py mpki no-legend logy &
python3 stacked-bar.py btb-miss-br-type &
python3 stacked-bar.py uncond-wss no-legend hline=5120 &
python3 stacked-bar.py btb-accesses &
python3 side-by-side-bar.py miss-coverage &
python3 side-by-side-bar.py speedup logy &
python3 side-by-side-bar.py shotgun-confluence-speedup logy &
python3 side-by-side-bar.py accuracy &
python3 multi-input.py &

python3 size-cap-miss.py size-capacity-misses
python3 assoc-con-miss.py assoc-conflict-misses

python3 size-cap-miss.py sensitivity-capacity logy show-legend &
python3 assoc-con-miss.py sensitivity-assoc show-legend &

python3 size-cap-miss.py sensitivity-pref-buffer-size logy show-legend &

wait
