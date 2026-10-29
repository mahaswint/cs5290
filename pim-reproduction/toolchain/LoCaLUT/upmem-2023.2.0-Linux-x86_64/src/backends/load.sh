#!/bin/bash
mkdir build;
cd build;
cmake ..;
make -j; 
echo "Success" $UPMEM_HOME;
cp hw/libdpuhw.so.0.0 $UPMEM_HOME/lib/libdpuhw.so.2023.2;
cp api/libdpu.so.0.0 $UPMEM_HOME/lib/libdpu.so.2023.2;
cp api/libdpu.so.0.0 $UPMEM_HOME/lib/libdpu.so.0.0;
cp ../api/include/api/dpu.h $UPMEM_HOME/include/dpu/dpu.h;
cp ../api/include/api/dpu_types.h $UPMEM_HOME/include/dpu/dpu_types.h;
