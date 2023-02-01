#!/bin/bash
# Copyright 2021 (c) Moritz Walker, ISW University of Stuttagart (for umati and VDW e.V.)

docker build -t opcuaclient .
docker container rm -f opcuaclient
docker run -it --rm -v /home/goetz/projects/Dashboard-OPCUA-Client/configuration.json:/app/configuration.json --name=opcuaclient opcuaclient
