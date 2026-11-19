#!/bin/sh
set -eu
K=$(uname -r); D=/usr/lib/modules/$K
O=/persist/a9b/backup/gpufreq-bringup-orig
S=/persist/a9b/.bringup-trial-state
cp -f $O/mtk_gpufreq_mt6835.ko $O/mtk_gpufreq_wrapper.ko $D/
echo REVERTED > $S; sync
md5sum $D/mtk_gpufreq_mt6835.ko $D/mtk_gpufreq_wrapper.ko
