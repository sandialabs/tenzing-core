#!/bin/bash
#BSUB -J mcts-halo-anticorr
#BSUB -o mcts-halo-anticorr.o%J
#BSUB -e mcts-halo-anticorr.e%J
#BSUB -W 2:00
#BSUB -nnodes 1

DIR=/ascldap/users/cwpears/repos/sched
OUT=$DIR/vortex/mcts-halo-anticorr.csv
EXE=$DIR/build-vortex/src_mcts_halo/mcts-halo-anticorr

source $DIR/load-env.sh

date
jsrun --smpiargs="-gpu" -n 4 -g 1 -c 1 -r 4 -l gpu-gpu,gpu-cpu -b rs $EXE | tee $OUT
date
