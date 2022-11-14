FROM debian:jessie

ENV PEQUIN /opt/pequin
ENV LITMUS /opt/litmus

RUN set -ex \
    && buildDeps=" \
    python2.7 libpython2.7 openjdk-7-jdk python3 \
    golang build-essential gcc-6 g++-6 gcc-6-multilib g++-6-multilib ant  \
    ant-optional make time libboost-all-dev libgmp10 libgmp-dev   \
    zlib1g zlib1g-dev libssl-dev cmake git pkg-config curl wget\
    libjemalloc-dev libntl-dev pkg-config numactl apt-utils " \
    && echo "deb http://ftp.de.debian.org/debian stretch main" >> /etc/apt/sources.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends $buildDeps \
    && curl -O https://download.java.net/java/GA/jdk14/076bab302c7b4508975440c56f6cc26a/36/GPL/openjdk-14_linux-x64_bin.tar.gz \
    && tar xvf openjdk-14_linux-x64_bin.tar.gz \
    && mv jdk-14 /opt/ \
    && echo "/opt/jdk-14/lib \n/opt/jdk-14/lib/server \n" > /etc/ld.so.conf.d/jdk-14.conf \
    && ldconfig

COPY dbx1000_logging/ $LITMUS

COPY pequin/ $PEQUIN
RUN cd $PEQUIN/thirdparty && ./install_pepper_deps.sh
RUN cd $PEQUIN && ./install_buffet.sh
RUN mv $PEQUIN/thirdparty/libsnark /tmp && rm -rf $PEQUIN/thirdparty/* \
    && mv /tmp/libsnark $PEQUIN/thirdparty/ \
    && rm -rf $PEQUIN/compiler/buffetfsm/llvm $PEQUIN/compiler/buffetfsm/llvm-build \

ENV LD_LIBRARY_PATH /opt/jdk-14/lib/server

CMD ["/bin/bash"]