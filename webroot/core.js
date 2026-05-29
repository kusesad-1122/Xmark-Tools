var intervalId = null;

function updateUI(statusLine) {
    let parts = statusLine.trim().split("|");
    let state = parts[0];
    let game = parts[1] || "";

    const indicator = document.getElementById("statusIndicator");
    const statusTextEl = document.getElementById("statusText");
    const gameInfoEl = document.getElementById("gameInfo");

    if (state === "ON") {
        indicator.className = "status-indicator on";
        statusTextEl.textContent = "防设备标记已开启";
        if (game) {
            gameInfoEl.innerHTML = `检测到 <strong>${game}</strong> 游戏已开启，已改空挂载`;
        } else {
            gameInfoEl.innerHTML = "正在监控中...";
        }
    } else {
        indicator.className = "status-indicator off";
        statusTextEl.textContent = "防设备标记未开启";
        gameInfoEl.innerHTML = "等待游戏启动...";
    }
}

function fetchStatus() {
    // 直接读模块目录下的状态文件，WebUI 有权限访问
    ksud.exec("cat /data/adb/modules/xinmaskplus/status", function(code, stdout, stderr) {
        if (code === 0 && stdout.trim()) {
            updateUI(stdout.trim());
        }
    });
}

window.addEventListener("load", function() {
    fetchStatus();
    intervalId = setInterval(fetchStatus, 2000);
});

window.addEventListener("beforeunload", function() {
    if (intervalId) clearInterval(intervalId);
});