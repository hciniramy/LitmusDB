FROM pequin:latest

# LitmusDB build arguments defied in dockerfile
ARG BUILD_REF="main"
ARG BUILD_ALG="serial"
ARG BUILD_CC_ALG="NO_WAIT"
ARG BUILD_MEM_INTEGRITY="RSA_AD"
ARG BUILD_VERIFICATION="false"
ARG BUILD_ELLE_OUTPUT="false"

# avoid annoying debconf output
ENV DEBIAN_FRONTEND noninteractive

# make files and scripts accessible inside the container
ENV LITMUSDB /root/LitmusDB

# expose the libjvm.so to the linker
ENV LD_LIBRARY_PATH /usr/lib/jvm/adoptopenjdk-14-hotspot-amd64/lib/server

COPY . $LITMUSDB

# checkout to  the required build version (see tags)
RUN cd $LITMUSDB && git checkout $BUILD_REF

# install dependencies
RUN $LITMUSDB/dbx1000_logging/install_deps.sh
RUN apt-get update && apt-get install -y --no-install-recommends --force-yes vim
RUN mkdir -p $LITMUSDB/pequin/thirdparty && cp -r /opt/pequin/thirdparty/libsnark $LITMUSDB/pequin/thirdparty/

# build LitmusDB
RUN cd $LITMUSDB/dbx1000_logging && python3 tools/compile.py $BUILD_ALG $BUILD_WORKlOAD $BUILD_CC_ALG LOG_DATA $BUILD_MEM_ITEGRITY $BUILD_VERIFICATION $BUILD_ELLE_OUTPUT
RUN cd $LITMUSDB/dbx1000_logging && mkdir logs results
  
