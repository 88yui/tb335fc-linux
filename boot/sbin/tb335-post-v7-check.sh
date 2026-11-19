#!/bin/bash
# C3~C5 验收：v7 补丁启动后，确认 probe 走完并用官方接口给 MFGSYS 上电
echo "===== 守卫状态 ====="; cat /persist/a9b/.bringup-trial-state 2>&1; echo
echo "===== 模块 md5 ====="; md5sum /usr/lib/modules/*/mtk_gpufreq_mt6835.ko /usr/lib/modules/*/mtk_gpufreq_wrapper.ko 2>&1
echo
echo "===== C3: /proc/gpufreqv2/ 是否出现（probe 走完的铁证）====="
ls -la /proc/gpufreqv2/ 2>&1
if [ -d /proc/gpufreqv2 ]; then
  echo ">>> C3 PASS: gpufreq_debug_init 已执行，probe 走完"
  echo "--- mfgsys_power_control（上电前）---"; cat /proc/gpufreqv2/mfgsys_power_control 2>&1
  echo "--- gpufreq_status（上电前）---"; cat /proc/gpufreqv2/gpufreq_status 2>&1
  echo "--- spmprobe MFG（上电前）---"; cat /proc/spmprobe 2>&1 | head -8
  echo
  echo "===== C4: 写 on -> gpufreq_power_control(POWER_ON,-1) ====="
  echo on > /proc/gpufreqv2/mfgsys_power_control 2>&1; echo "write rc=$?"
  sleep 3
  echo "--- 读回 mfgsys_power_control ---"; cat /proc/gpufreqv2/mfgsys_power_control 2>&1
  echo "--- gpufreq_status（上电后）---"; cat /proc/gpufreqv2/gpufreq_status 2>&1
  echo "--- spmprobe MFG（上电后）---"; cat /proc/spmprobe 2>&1 | head -8
  echo
  echo "===== C5: /dev/mali0 ====="; ls -la /dev/mali0 2>&1
  echo "--- dmesg 关键行 ---"; dmesg 2>/dev/null | grep -aiE "fail to power on MFGSYS|gpufreq|mali" | tail -12
else
  echo ">>> C3 FAIL: gpufreqv2 未出现，probe 仍未走完"
  echo "--- spmprobe MFG ---"; cat /proc/spmprobe 2>&1 | head -8
  echo "--- 卡点栈 ---"; cat /proc/1/task/*/stack 2>/dev/null | grep -a gpufreq | head -5
fi
echo "===== DONE ====="
