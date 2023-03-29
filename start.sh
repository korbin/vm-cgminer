sudo modprobe ftdi_sio
./cgminer --disable-gpu -S /dev/ttyUSB2 -S /dev/ttyUSB6 -S /dev/ttyUSB10 -S /dev/ttyUSB14 -S /dev/ttyUSB18 -S /dev/ttyUSB22 -S /dev/ttyUSB26 -S /dev/ttyUSB30 --icarus-timing 0.178 --cainsmore-clock 625 -o stratum+tcp://stratum-eu.rplant.xyz:7086 -u 1HsKNeobhZvQRb3KQhet2PKnb79gwWuu6q.78 -p start=64 > out.txt
