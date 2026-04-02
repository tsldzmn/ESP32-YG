const express = require('express');
const cors = require('cors');
const path = require('path');
const app = express();

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ========== 数据存储 ==========
let sensorData = {
  waterLevel: 75,
  waterTemp: 25.5,
  turbidity: 15,
  lastUpdate: new Date().toISOString()
};

let deviceStatus = {
  light: false,
  feeder: false
};

// 系统日志
let systemLogs = [
  { id: 1, type: 'warn', text: '温度飙升至 26.5°C', time: '10分钟前' },
  { id: 2, type: 'info', text: '自动喂食器已成功投食', time: '2小时前' },
  { id: 3, type: 'error', text: '过滤泵流量下降', time: '昨天' }
];
let nextLogId = 4;

// 待办维护
let maintItems = [
  { id: 1, icon: '💧', iconClass: 'water', task: '换水20%', due: '2天后到期' },
  { id: 2, icon: '🧹', iconClass: 'clean', task: '清理蛋白质分离器', due: '明天到期' }
];
let nextMaintId = 3;

// 历史数据
let historyData = [];
for (let i = 24; i >= 0; i--) {
  const time = new Date(Date.now() - i * 3600000);
  historyData.push({
    time: time.toISOString(),
    waterLevel: 70 + Math.random() * 15,
    waterTemp: 24 + Math.random() * 3,
    turbidity: 10 + Math.random() * 20
  });
}

// ========== 传感器数据 API ==========

// 获取传感器数据
app.get('/api/sensor', (req, res) => {
  sensorData.lastUpdate = new Date().toISOString();
  res.json({ success: true, data: sensorData });
});

// ESP32 上报数据
app.post('/api/esp32/report', (req, res) => {
  const { waterLevel, waterTemp, turbidity } = req.body;
  const oldTemp = sensorData.waterTemp;
  if (waterLevel !== undefined) sensorData.waterLevel = waterLevel;
  if (waterTemp !== undefined) sensorData.waterTemp = waterTemp;
  if (turbidity !== undefined) sensorData.turbidity = turbidity;
  sensorData.lastUpdate = new Date().toISOString();

  // 自动记录日志
  if (waterTemp !== undefined && waterTemp > 28) {
    addLog('warn', `温度过高报警：${waterTemp}°C`);
  }
  if (waterLevel !== undefined && waterLevel < 30) {
    addLog('error', `水位过低：${waterLevel}%`);
  }
  if (turbidity !== undefined && turbidity > 30) {
    addLog('warn', `浑浊度偏高：${turbidity} NTU`);
  }

  historyData.push({
    time: sensorData.lastUpdate,
    waterLevel: sensorData.waterLevel,
    waterTemp: sensorData.waterTemp,
    turbidity: sensorData.turbidity
  });
  if (historyData.length > 48) historyData = historyData.slice(-48);

  res.json({ success: true, message: '数据已接收' });
});

// ========== 历史数据 API ==========

app.get('/api/history', (req, res) => {
  res.json({ success: true, data: historyData });
});

// ========== 设备控制 API ==========

// 获取设备状态
app.get('/api/device', (req, res) => {
  res.json({ success: true, data: deviceStatus });
});

// 灯光控制
app.post('/api/light', (req, res) => {
  const { status } = req.body;
  deviceStatus.light = status;
  addLog('info', status ? 'LED灯光已开启' : 'LED灯光已关闭');
  res.json({ success: true, data: deviceStatus });
});

// 喂食
app.post('/api/feeder', (req, res) => {
  deviceStatus.feeder = true;
  addLog('info', '自动喂食器开始投食');
  setTimeout(() => {
    deviceStatus.feeder = false;
    addLog('info', '自动喂食器投食完成');
  }, 5000);
  res.json({ success: true, data: deviceStatus });
});

// 喂食器定时开关
app.post('/api/feeder/toggle', (req, res) => {
  const { status } = req.body;
  addLog('info', status ? '自动喂食器已开启' : '自动喂食器已关闭');
  res.json({ success: true });
});

// ========== 系统日志 API ==========

function addLog(type, text) {
  systemLogs.unshift({
    id: nextLogId++,
    type,
    text,
    time: '刚刚'
  });
  if (systemLogs.length > 50) systemLogs = systemLogs.slice(0, 50);
}

app.get('/api/logs', (req, res) => {
  res.json({ success: true, data: systemLogs });
});

app.post('/api/logs', (req, res) => {
  const { type, text } = req.body;
  addLog(type || 'info', text);
  res.json({ success: true });
});

// ========== 待办维护 CRUD ==========

app.get('/api/maintenance', (req, res) => {
  res.json({ success: true, data: maintItems });
});

app.post('/api/maintenance', (req, res) => {
  const { task, due, icon, iconClass } = req.body;
  const item = {
    id: nextMaintId++,
    icon: icon || '🔧',
    iconClass: iconClass || 'clean',
    task: task || '未命名事项',
    due: due || '未设置'
  };
  maintItems.push(item);
  addLog('info', `添加维护事项：${item.task}`);
  res.json({ success: true, data: item });
});

app.put('/api/maintenance/:id', (req, res) => {
  const id = parseInt(req.params.id);
  const item = maintItems.find(i => i.id === id);
  if (!item) return res.status(404).json({ success: false, message: '事项不存在' });
  if (req.body.task !== undefined) item.task = req.body.task;
  if (req.body.due !== undefined) item.due = req.body.due;
  if (req.body.icon !== undefined) item.icon = req.body.icon;
  if (req.body.iconClass !== undefined) item.iconClass = req.body.iconClass;
  res.json({ success: true, data: item });
});

app.delete('/api/maintenance/:id', (req, res) => {
  const id = parseInt(req.params.id);
  const idx = maintItems.findIndex(i => i.id === id);
  if (idx === -1) return res.status(404).json({ success: false, message: '事项不存在' });
  const removed = maintItems.splice(idx, 1)[0];
  addLog('info', `删除维护事项：${removed.task}`);
  res.json({ success: true });
});

// ========== ESP32 指令下发 ==========
app.get('/api/esp32/command', (req, res) => {
  res.json({
    success: true,
    data: {
      light: deviceStatus.light,
      feeder: deviceStatus.feeder
    }
  });
});

// ========== 模拟数据波动（开发/演示用） ==========
setInterval(() => {
  sensorData.waterLevel = Math.max(10, Math.min(100, sensorData.waterLevel + (Math.random() - 0.5) * 2));
  sensorData.waterTemp = Math.max(18, Math.min(32, sensorData.waterTemp + (Math.random() - 0.5) * 0.3));
  sensorData.turbidity = Math.max(0, Math.min(50, sensorData.turbidity + (Math.random() - 0.5) * 2));
  sensorData.lastUpdate = new Date().toISOString();
}, 5000);

// 每30分钟自动记录一次历史
setInterval(() => {
  historyData.push({
    time: new Date().toISOString(),
    waterLevel: sensorData.waterLevel,
    waterTemp: sensorData.waterTemp,
    turbidity: sensorData.turbidity
  });
  if (historyData.length > 48) historyData = historyData.slice(-48);
}, 30 * 60 * 1000);

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log('🐟 ESP32 智能鱼缸监控面板已启动');
  console.log(`📊 http://localhost:${PORT}`);
});
