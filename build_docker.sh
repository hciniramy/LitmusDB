#!/bin/bash

set -e
 
# build pequin image if it does not exists
if [[ "$(docker images -q pequin:latest 2> /dev/null)" == "" ]]; then
  echo "[Script-INFO] Pequin docker image does not exists, building it"
  cd pequin
  ./build_docker.sh
  cd ..
else
  echo "[Script-INFO] Pequin docker image already exists"
fi

echo "[Script-INFO] Building litmus docker image"
docker build -t litmusdb -f docker/Dockerfile .

