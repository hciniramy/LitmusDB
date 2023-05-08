FROM pequin_jessie:latest

# LitmusDB build arguments defined in dockerfile
    # $1
ARG BUILD_REF="main"
    # $2
ARG BUILD_ALG="serial"
    # $3
ARG BUILD_CC_ALG="NO_WAIT"
    # $4
ARG BUILD_WORKLOAD="TPCC"
    # 5
ARG BUILD_MEM_INTEGRITY="RSA_AD"
    # $6
ARG BUILD_VERIFICATION="false"
    # $7
ARG BUILD_ELLE_OUTPUT="false"

# avoid annoying debconf output
ENV DEBIAN_FRONTEND noninteractive

# make files and scripts accessible inside the container
ENV LITMUSDB /root/LitmusDB

# expose the libjvm.so to the linker
ENV LD_LIBRARY_PATH /usr/lib/jvm/adoptopenjdk-14-hotspot-amd64/lib/server

COPY . $LITMUSDB

# checkout to  the required build version (see tags)
RUN cd $LITMUSDB # && git checkout $BUILD_REF

# install dependencies
RUN $LITMUSDB/dbx1000_logging/install_deps.sh
RUN apt-get update && apt-get install -y --no-install-recommends --force-yes vim
RUN mkdir -p $LITMUSDB/pequin/thirdparty && cp -r /opt/pequin/thirdparty/libsnark $LITMUSDB/pequin/thirdparty/

# build LitmusDB
RUN cd $LITMUSDB/dbx1000_logging && python3 tools/compile.py $BUILD_ALG $BUILD_WORKLOAD $BUILD_CC_ALG LOG_DATA $BUILD_MEM_INTEGRITY $BUILD_VERIFICATION $BUILD_ELLE_OUTPUT
RUN cd $LITMUSDB/dbx1000_logging && mkdir logs results
  
