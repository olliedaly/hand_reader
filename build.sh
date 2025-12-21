#!/bin/bash
pio run > build.log 2>&1
echo "Done" >> build.log
