const express = require('express');
const cors = require('cors');
const path = require('path');
const app = express();

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ========== 数据存储 ==========
let sensorData = {
  waterLevel: 0,
  waterTemp: 0,
  turbidity: 0,
  lastUpdate: new Date().toISOString()
};

let deviceStatus = {
  light: false,
  feeder: false
};

// 系统日志
let systemLogs = [];
let nextLogId = 1;

// 待办维护
let maintItems = [];
let nextMaintId = 1;

// 历史数据
let historyData = [];

// ========== 传感器数据 API ==========

// 获取传感器数据
app.get('/api/sensor', (req, res) => {
  sensorData.lastUpdate = new Date().toISOString();
  res.json({ success: true, data: sensorData });
});

// ESP32 上报数据
app.post('/api/esp32/report', (req, res) => {
  const { waterLevel, waterTemp, turbidity } = req.body;
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

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log('ESP32 智能鱼缸监控面板已启动');
  console.log(`http://localhost:${PORT}`);
});
