#!/system/bin/sh
############################################
# xinmaskplus metainstall.sh
# 元模块(magic_mount_rs)安装钩子
############################################
export KSU_HAS_METAMODULE="true"
export KSU_METAMODULE="xinmaskplus"
ui_print "- xinmaskplus: 元模块安装流程"
handle_partition() { true; }
mark_replace() {
    mkdir -p "$1"
    setfattr -n trusted.overlay.opaque -v y "$1" 2>/dev/null
}
install_module
mm_handle_partition() {
    [ ! -d "$MODPATH/system/$1" ] && return
    if [ -L "/system/$1" ] && [ -d "/$1" ]; then
        ui_print "- 处理分区 /$1"
        ln -sf "./system/$1" "$MODPATH/$1"
    fi
}
mm_handle_partition system_ext
mm_handle_partition vendor
mm_handle_partition product
ui_print "- xinmaskplus 安装完成"
