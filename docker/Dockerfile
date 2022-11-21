FROM pequin:latest

# avoid annoying debconf output
ENV DEBIAN_FRONTEND noninteractive
# make files and scripts accessible inside the container
ENV LITMUSDB /root/LitmusDB
COPY . $LITMUSDB
# install dependencies
RUN $LITMUSDB/dbx1000_logging/install_deps.sh
RUN apt update && apt install -y --no-install-recommends vim
ENV LD_LIBRARY_PATH /usr/lib/jvm/adoptopenjdk-14-hotspot-amd64/lib/server

RUN mkdir -p $LITMUSDB/pequin/thirdparty && cp -r /opt/pequin/thirdparty/libsnark $LITMUSDB/pequin/thirdparty/

# build LitmusDB
RUN cd $LITMUSDB/dbx1000_logging && python3 tools/compile.py
RUN cd $LITMUSDB/dbx1000_logging && mkdir logs results
  
