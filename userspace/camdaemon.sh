#!/bin/sh
# camdaemon v9: 双 V4L2 节点（camv4l2=后摄 / camv4l2-front=前置）
# 应用在设备列表里选哪个节点，就出哪颗摄像头的画面（应用内即可切换）
# 老应用兼容：camswitch front|rear 只改 /dev/video0 软链指向，下一次开流生效
LOG=/var/log/camdaemon.log
log(){ echo "$(date '+%H:%M:%S') $*" >> $LOG; }
REAR_NODE=""; FRONT_NODE=""

ensure_nodes(){
  if ! grep -q "^camv4l2 " /proc/modules; then
    insmod /root/camv4l2.ko video_nr=3 2>>$LOG && log "insmod camv4l2 ok"
    sleep 0.3
  fi
  for n in /sys/class/video4linux/video*; do
    [ -e "$n/name" ] || continue
    nm=$(cat "$n/name" 2>/dev/null)
    case "$nm" in
      camv4l2|camv4l2-front)
        dv=$(cat "$n/dev")
        maj=$(echo "$dv" | cut -d: -f1); min=$(echo "$dv" | cut -d: -f2); idx=$(echo "$n" | sed 's/.*video//')
        [ -c "/dev/video$idx" ] || mknod "/dev/video$idx" c "$maj" "$min"
        chmod 666 "/dev/video$idx"
        if [ "$nm" = "camv4l2-front" ]; then FRONT_NODE=$idx; else REAR_NODE=$idx; fi
        log "node /dev/video$idx $dv ($nm)"
        ;;
    esac
  done
  cdv=$(cat /sys/class/misc/camfeed/dev 2>/dev/null)
  if [ -n "$cdv" ]; then
    cmaj=$(echo "$cdv" | cut -d: -f1); cmin=$(echo "$cdv" | cut -d: -f2)
    [ -c /dev/camfeed ] || mknod /dev/camfeed c "$cmaj" "$cmin"
    chmod 666 /dev/camfeed
  fi
  if [ -f /root/w303_setup_all.sh ]; then sh /root/w303_setup_all.sh >/dev/null 2>&1; fi
  if ! grep -q camreg_isp /proc/misc; then
    insmod /root/camreg_isp_top.ko devname=1a030000.cam1_legacy 2>>$LOG && log "insmod camreg ok"
  fi
  cmin=$(awk '/camreg_isp/{print $1}' /proc/misc)
  if [ -n "$cmin" ]; then
    rm -f /dev/camreg
    mknod /dev/camreg c 10 "$cmin"
    chmod 666 /dev/camreg
  fi
  if ! grep -q "^afreg " /proc/modules; then
    insmod /root/afreg.ko bus=8 addr=0x0c 2>>$LOG && log "insmod afreg ok"
  fi
  chmod 666 /proc/driver/camsensor 2>/dev/null
  chmod 666 /proc/driver/camsensor2 2>/dev/null
}

reload_camreg(){
  rmmod camreg_isp 2>/dev/null
  sleep 0.1
  insmod /root/camreg_isp_top.ko devname=1a030000.cam1_legacy 2>>$LOG || true
  sleep 0.2
  cmin=$(awk '/camreg_isp/{print $1}' /proc/misc)
  if [ -n "$cmin" ]; then
    rm -f /dev/camreg
    mknod /dev/camreg c 10 "$cmin"
    chmod 666 /dev/camreg
  fi
}

log "=== camdaemon v9 start (dual node) ==="
: > /var/log/camvid_rear.log 2>/dev/null
: > /var/log/camvid_front.log 2>/dev/null
ensure_nodes
log "nodes: rear=/dev/video${REAR_NODE:-?} front=/dev/video${FRONT_NODE:-?}"
CAM_PID=0
CAM_MODE=""
LAST_FMT=""

stop_pipeline(){
  [ "$CAM_PID" = "0" ] && { CAM_MODE=""; return; }
  kill -TERM "$CAM_PID" 2>/dev/null
  i=0
  while [ $i -lt 12 ]; do
    kill -0 "$CAM_PID" 2>/dev/null || break
    sleep 0.5
    i=$((i+1))
  done
  kill -KILL "$CAM_PID" 2>/dev/null
  wait "$CAM_PID" 2>/dev/null
  CAM_PID=0; CAM_MODE=""
  log "camera pipeline stopped"
}

start_pipeline(){
  mode="$1"
  reload_camreg
  if [ "$mode" = "front" ]; then
    log "app streaming front node -> start FRONT pipeline"
    env $HF_F CAMREC_VFAST=1 CAMREC_TNR_ALPHA=0.45 CAMREC_HI_FLOOR=175 /root/camfront -n 100000 -L /dev/camfeed -F 30 -v 2000 -e 1900 -g 0x10 >> /var/log/camvid_front.log 2>&1 &
    CAM_PID=$!; CAM_MODE=front
    log "camfront started pid=$CAM_PID"
  else
    log "app streaming rear node -> start REAR pipeline (v8 tone/night)"
    env $HF_R CAMREC_VFAST=1 CAMREC_TNR_ALPHA=0.45 CAMREC_HI_FLOOR=175 CAMREC_AE_VTSMIN=2500 CAMREC_AE_VTSMAX=6000 CAMREC_VTHREADS=2 /root/camvid -n 100000 -L /dev/camfeed -F 30 -v 6000 -e 2400 -g 0 -A -t 330 --vcontrast 1.25 --vgamma 2.0 --af --af-iv 30 >> /var/log/camvid_rear.log 2>&1 &
    CAM_PID=$!; CAM_MODE=rear
    log "camvid started pid=$CAM_PID"
  fi
  LAST_FMT=$(/root/camfeedfmt 2>/dev/null)
}

while :; do
  WHICH=$(cat /persist/cam_which 2>/dev/null)
  [ "$WHICH" = "front" ] || WHICH=rear
  FLIP=$(cat /persist/cam_flip 2>/dev/null)
  [ -z "$FLIP" ] && FLIP=front
  HF_F=""; HF_R=""
  case "$FLIP" in
    front) HF_F="CAMREC_HFLIP=1" ;;
    rear)  HF_R="CAMREC_HFLIP=1" ;;
    both)  HF_F="CAMREC_HFLIP=1"; HF_R="CAMREC_HFLIP=1" ;;
  esac
  # 兼容老应用：/dev/video0 跟随 camswitch 指向
  if [ "$WHICH" = "front" ] && [ -n "$FRONT_NODE" ]; then
    ln -sf "/dev/video$FRONT_NODE" /dev/video0
  elif [ -n "$REAR_NODE" ]; then
    ln -sf "/dev/video$REAR_NODE" /dev/video0
  fi

  S=0
  if [ -c /dev/camfeed ]; then
    LINE=$(/root/camfeedstat 2>/dev/null)
    case "$LINE" in
      ""|-1*) S=0 ;;
      *) S=$(echo "$LINE" | cut -d" " -f1) ;;
    esac
  fi
  WANT=""
  case "$S" in
    1) WANT=rear ;;
    2) WANT=front ;;
  esac

  if [ "$CAM_PID" != "0" ] && ! kill -0 "$CAM_PID" 2>/dev/null; then
    log "camera pipeline exited (pid $CAM_PID, mode $CAM_MODE)"
    CAM_PID=0; CAM_MODE=""
  fi

  if [ -n "$WANT" ]; then
    if [ "$CAM_MODE" != "$WANT" ]; then
      [ "$CAM_PID" != "0" ] && { log "switch $CAM_MODE -> $WANT"; stop_pipeline; }
      start_pipeline "$WANT"
    fi
  else
    [ "$CAM_PID" != "0" ] && { log "app stopped"; stop_pipeline; }
  fi
  # 应用改分辨率/格式时，重启管线让 camvid/camfront 重新学习 feed 格式
  if [ "$CAM_PID" != "0" ] && [ -n "$WANT" ]; then
    FMT=$(/root/camfeedfmt 2>/dev/null)
    if [ -n "$FMT" ] && [ "$FMT" != "$LAST_FMT" ]; then
      log "app format changed ($LAST_FMT -> $FMT) -> restart pipeline"
      stop_pipeline
      start_pipeline "$WANT"
    fi
  fi
  sleep 1
done
