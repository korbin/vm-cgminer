#!/bin/bash

address=1HsKNeobhZvQRb3KQhet2PKnb79gwWuu6q
machine=$(echo $HOSTNAME | awk '{print substr($1,5,4)}')
clock=500
starting_diff=64

./cgminer --disable-gpu -S /dev/ttyUSB2 -S /dev/ttyUSB6 -S /dev/ttyUSB10 \
-S /dev/ttyUSB14 -S /dev/ttyUSB18 -S /dev/ttyUSB22 -S /dev/ttyUSB26 -S /dev/ttyUSB30 \
-S /dev/ttyUSB34 -S /dev/ttyUSB38 -S /dev/ttyUSB42 -S /dev/ttyUSB46 -S /dev/ttyUSB50 \
-S /dev/ttyUSB54 -S /dev/ttyUSB58 -S /dev/ttyUSB62 -S /dev/ttyUSB66 -S /dev/ttyUSB70 \
-S /dev/ttyUSB74 -S /dev/ttyUSB78 -S /dev/ttyUSB82 -S /dev/ttyUSB86 -S /dev/ttyUSB90 \
-S /dev/ttyUSB94 -S /dev/ttyUSB98 -S /dev/ttyUSB102 -S /dev/ttyUSB106 -S /dev/ttyUSB110 \
-S /dev/ttyUSB114 -S /dev/ttyUSB118 -S /dev/ttyUSB122 -S /dev/ttyUSB126 -S /dev/ttyUSB130 \
-S /dev/ttyUSB134 -S /dev/ttyUSB138 -S /dev/ttyUSB142 -S /dev/ttyUSB146 -S /dev/ttyUSB150 \
--icarus-timing 0.17 -o stratum+tcp://usmw.vipor.net:5067 -u ${address}.${machine} \
-p start=${starting_diff} --cainsmore-clock ${clock}

