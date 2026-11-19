#!/bin/sh
# tb335-load-modules.sh —— 顺序显式加载内核驱动（替代 udev 并行冷插拔）
#
# 背景（P12 §16）：本机在"udev 开机自动加载模块"时会于 uptime ~17-20s 挂死复位，
#   但**逐个顺序 modprobe 全部正常**（126 个模块逐个验证 rc=0）。因此改为顺序显式加载。
#
# 自愈设计：
#   * 每个模块加载前把它写入 mod-inprogress 并 sync；成功后删除。
#   * 若加载某模块导致设备复位，下次启动会看到 mod-inprogress 仍在 => 把它加入 modskip.txt 并跳过。
#   * 因此即使某个模块真的会崩，系统也能自我修复并继续加载其余模块。
#
# ============================================================================
# v2（2026-10-15，P16e 实验版）—— "对齐原厂"
#   依据：从原厂 vendor_dlkm_a.img 解出的权威 modules.load（158 条）
#     原厂 GPU 链位置：#28 gpueb -> #29 mtk_gpu_hal -> #30 mtk_gpufreq_wrapper
#                       -> #31 mtk_gpufreq_mt6835 -> #32 ged -> #33 mali_kbase_mt6835
#                       #45 mtk_gpu_qos   #124 mcupm   #99 fhctl
#   本版改动（仅三处，其余保持原样以免引入无关变量）：
#     ① 把 mtk_gpu_hal / mtk_gpu_qos / mtk_gpufreq_wrapper 从早期位置移到末尾 GPU 块；
#     ② 末尾构建**原厂顺序的 GPU 连续块**，并让 fhctl 排在其**之后**
#        （原厂设计：先让 gpufreq 加载并 deferred，等 fhctl 绑定后再由 fw_devlink 触发 probe，
#          从而保证 ged / mali_kbase 等下游设备已在位）；
#     ③ 补入原厂有、本机未加载的 30 个模块（显示/面板 11 + GPU/电源 19）。
#   回滚：cp /persist/a9b/tb335-load-modules.sh.bak-* /usr/local/sbin/tb335-load-modules.sh
# ============================================================================
LOG=/persist/a9b/modload.log
IN=/persist/a9b/mod-inprogress
SKIP=/persist/a9b/modskip.txt
mkdir -p /persist/a9b 2>/dev/null
touch "$SKIP" 2>/dev/null
if [ -s "$IN" ]; then
    M=$(cat "$IN" 2>/dev/null)
    echo "!! 上次启动崩在: ${M:-?} -> 已加入跳过名单" >> "$LOG"
    [ -n "$M" ] && echo "$M" >> "$SKIP"
    rm -f "$IN"
    sync
fi
echo "===== BOOT uptime=$(cut -d' ' -f1 /proc/uptime 2>/dev/null) modules=$(wc -l < /proc/modules) =====" >> "$LOG"
sync
OK=0; FAIL=0; SKIPN=0
ELAPSED0=$(cut -d'.' -f1 /proc/uptime)
for m in 8250_mtk adapter_class adsp aee_aed arm_dsu_pmu audio_ipi audio_spkr_amp_mngr backlight_cooling blocktag board_temp bootprof camera_eeprom charger_class charger_cooling clk_common clk_disable_unused clkbuf cmdq_sec_drv cmdq_test cqhci device_apc_common emi emi_mpu extcon_mtk_usb extdev_io_class flashlight flashlights_aw3641e gpueb gz_ipc_mod gz_main_mod gz_trusty_mod gz_tz_system gz_virtio_mod hqsysfs i2c_mt65xx imgsensor_isp6s iommu_debug iommu_secure irq_dbg leds_gpio leds_mtk leds_mtk_pwm lenovo_keyboard mcDrvModule mm8013 mmprofile mmqos_common mmqos_mt6835 mrdump mt6315_regulator mt6375 mt6377_accdet mt6377_clkbuf mt6377_regulator mt6577_auxadc mtk_afe_external mtk_battery_oc_throttling mtk_bp_thl mtk_btcvsd mtk_charger_algorithm_class mtk_charger_framework mtk_chg_type_det mtk_cmdq_drv_ext mtk_composite mtk_cpu_power_throttling mtk_dramc mtk_dvfsrc mtk_dvfsrc_regulator mtk_dynamic_loading_throttling mtk_emi mtk_icc_core mtk_iommu mtk_jpeg mtk_low_battery_throttling mtk_lpm mtk_lpm_dbg_common_v1 mtk_mbox mtk_mdpm mtk_mmc mtk_mmc_dbg mtk_mmdvfs mtk_mmdvfs_debug mtk_mmdvfs_ftrace mtk_mmdvfs_v3 mtk_mml mtk_panel_ext mtk_pbm mtk_pd_adapter mtk_pd_charging mtk_pep mtk_pep20 mtk_pep45 mtk_pm_domain_disable_unused mtk_pmic_keys mtk_pmic_oc_debug mtk_pwm mtk_qos mtk_rpmsg_mbox mtk_scp_audio mtk_scp_audiocommon mtk_scp_ultra mtk_scpsys rt5133_regulator mediatek-drm-gateic mediatek-drm-panel-drv leds-mtk-disp mtk_irtx_pwm mtk_disp_sec mtk-mminfra-debug panel-boe-sycamore-90hz-drv panel-boeips-sycamore-90hz-drv panel-tcl-sycamore-90hz-drv panel-tianma-sycamore-90hz-drv panel-tmips-sycamore-90hz-drv cmdq_platform_mt6835 cmdq_helper_inf mtk-dvfsrc-start mtk_sec_heap mtk_slbc mtk_smi mtk_smi_dbg mtk_sp_spk_amp mtk_spmi_pmic_adc mtk_sync mtk_tinysys_ipi mtk_uart_apdma mtk_vcodec_common mtk_vcodec_dec_v1 mtk_vcodec_enc_v1 mtk_wdt mtu3 mux_switch nvmem_mtk_devinfo pd_dbg_info phy_mtk_tphy pinctrl_mtk_common_v2_debug pinctrl_mtk_v2 pmic_lbat_service pmic_lvsys_notify pmic_temp pmic_tia ps5169 reboot_mode reset_ti_syscon richtek_spm_cls rpmb rpmb_mtk rt_pd_manager rtc_mt6685 sc8960x_charger sc8960x_slave_charger scp sec_rng sgm415xx_charger sgm415xx_slave_charger snd_soc_audiodsp_common snd_soc_aw87xxx snd_soc_fs158x snd_soc_mt6377 snd_soc_mt6835_afe snd_soc_mtk_common snd_soc_mtk_scp_ultra snd_soc_rt5512 soc_temp_lvts spi_mt65xx spi_slave spmi_mtk_mpu spmi_mtk_pmif spmtwam sspm_v3 sycamore_i2c syscon_reboot_mode system_heap tcpc_class thermal_generic_adc thermal_interface tinysys_scmi trusted_mem uarthub_drv ufs_mediatek_dbg ufs_mediatek_mod usb_boost usb_meta v4l2_async vcp vcp_status widevine_driver xhci_mtk_hcd_v2 wlan_drv_gen4m_6835 btif_drv bt_drv_connac1x camera_isp camera_dip_isp6s camera_fdvt_isp51 mtk_gpu_hal mtk_gpufreq_wrapper mtk_gpufreq_mt6835 ged mali_kbase_mt6835 mali_mgm_mt6835 mali_prot_alloc_mt6835 mtk_gpu_qos mcupm fhctl nvmem-mt635x-efuse mtk_cm_ipi mtk_cm_mgr mtk_cm_mgr_mt6835 mtk-lpm-plat-v1 mtk-lpm-dbg-mt6835 mtk_core_ctl cpuqos_v3 cpudvfs bus-parity devmpu mtk-swpm mtk-swpm-dbg-common-v1 mtk-swpm-dbg-v6835 mtk-swpm-mem-dbg-v6835 mtk-swpm-core-dbg-v6835; do
    if [ -s "$SKIP" ] && grep -qx "$m" "$SKIP" 2>/dev/null; then SKIPN=$((SKIPN+1)); continue; fi
    echo "$m" > "$IN"
    sync -f "$IN" 2>/dev/null
    out=$(modprobe "$m" 2>&1); rc=$?
    rm -f "$IN"
    if [ "$rc" -eq 0 ]; then
        OK=$((OK+1)); echo "OK   $m" >> "$LOG"
    else
        FAIL=$((FAIL+1)); echo "FAIL $m rc=$rc :: $out" >> "$LOG"
    fi
done
ELAPSED1=$(cut -d'.' -f1 /proc/uptime)
echo "===== loader elapsed=$((ELAPSED1-ELAPSED0))s (was ~21s with per-module sync+sleep)" >> "$LOG"
echo "===== DONE ok=$OK fail=$FAIL skip=$SKIPN in-kernel=$(wc -l < /proc/modules) =====" >> "$LOG"

# ---- GPU 链自检（安全版：不含 kbase）----
echo "---- GPU chain check $(date -u '+%F %T' 2>/dev/null) ----" >> "$LOG"
echo "  fhctl        : $([ -d /sys/module/fhctl ] && echo loaded || echo MISSING)" >> "$LOG"
echo "  gpufreq bind : $(ls /sys/bus/platform/drivers/gpufreq/ 2>/dev/null | grep -vcE '^(bind|unbind|module|uevent)$')" >> "$LOG"
echo "  ged bind     : $(ls /sys/bus/platform/drivers/ged/ 2>/dev/null | grep -vcE '^(bind|unbind|module|uevent)$')" >> "$LOG"
echo "  kbase 加载   : $([ -d /sys/module/mali_kbase_mt6835 ] && echo loaded || echo NOT-loaded-by-design)" >> "$LOG"
echo "  mali0        : $([ -e /dev/mali0 ] && echo PRESENT || echo absent)" >> "$LOG"
cat /sys/kernel/debug/devices_deferred 2>/dev/null | sed "s/^/  deferred: /" >> "$LOG"
sync
sync
exit 0

# ---- mali0 设备节点（内核已创建 /sys/class/misc/mali0，但 udev 未生成 /dev 节点）----
if [ -e /sys/class/misc/mali0/dev ] && [ ! -e /dev/mali0 ]; then
    D=$(cat /sys/class/misc/mali0/dev)
    mknod /dev/mali0 c $(echo $D | cut -d: -f1) $(echo $D | cut -d: -f2) 2>/dev/null
    chmod 666 /dev/mali0 2>/dev/null
    echo "mknod /dev/mali0 done ($D)" >> /persist/a9b/modload.log
fi
