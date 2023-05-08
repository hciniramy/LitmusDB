#!/bin/bash

set -e
 
# build pequin image if it does not exists
if [[ "$(docker images -q pequin_jessie:latest 2> /dev/null)" == "" ]]; then
  echo "[Script-INFO] Pequin docker image does not exists, building it"
  cd pequin
  ./build_docker.sh
  cd ..
else
  echo "[Script-INFO] Pequin docker image already exists"
fi

echo "[Script-INFO] Building litmus docker image"
docker build -t $8  \
  --build-arg BUILD_REF=$1 \
  --build-arg BUILD_ALG=$2 \
  --build-arg BUILD_CC_ALG=$3 \
  --build-arg BUILD_WORKLOAD=$4 \
  --build-arg BUILD_MEM_INTEGRITY=$5 \
  --build-arg BUILD_VERIFICATION=$6 \
  --build-arg BUILD_ELLE_OUTPUT=$7 \
  -f docker/Dockerfile .

