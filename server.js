const express = require("express");
const http = require("http");
const cors = require("cors");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);

const io = new Server(server, {
  cors: {
    origin: "*",
    methods: ["GET", "POST"]
  }
});

app.use(cors());
app.use(express.json());
app.use(express.static("public"));

const PORT = process.env.PORT || 3000;

// =====================================================
// ESP32 CONFIG
// IMPORTANT: Only BASE IP here. Do NOT add /status.
// Example: http://10.120.45.25
// =====================================================

let ESP32_URL =
  process.env.ESP32_URL || "http://10.120.45.1";


// =====================================================
// DEFAULT DASHBOARD SCHEDULE
// =====================================================

let schedule = [
  {
    chamber: 1,
    medicine: "Medicine A",
    time: "08:00",
    dose: "1 tablet"
  },
  {
    chamber: 2,
    medicine: "Medicine B",
    time: "12:00",
    dose: "1 tablet"
  },
  {
    chamber: 3,
    medicine: "Medicine C",
    time: "16:00",
    dose: "1 tablet"
  },
  {
    chamber: 4,
    medicine: "Medicine D",
    time: "20:00",
    dose: "1 tablet"
  }
];


// =====================================================
// DEVICE STATE
// =====================================================

let lastDevice = {
  online: false,
  device: "ESP32-001",
  ip: getESP32IP(),
  wifi: false,
  time: null,
  date: null,
  timezone: "IST",
  utcOffset: "+05:30",
  chambers: [],
  lastSeen: null
};


// =====================================================
// DAILY TAKEN STATE
// =====================================================

let takenToday = new Set();


// =====================================================
// HELPERS
// =====================================================

function getESP32IP() {
  return ESP32_URL
    .replace(/^https?:\/\//, "")
    .replace(/\/$/, "");
}


function normalizeESP32URL(url) {
  if (!url) return null;

  url = String(url).trim();

  url = url.replace(/\/+$/, "");

  if (!/^https?:\/\//i.test(url)) {
    url = "http://" + url;
  }

  return url;
}


function statusForDose(item, now = new Date()) {
  const [h, m] = item.time.split(":").map(Number);

  const scheduled = new Date(now);

  scheduled.setHours(h, m, 0, 0);

  if (takenToday.has(item.chamber)) {
    return "TAKEN";
  }

  if (now.getTime() > scheduled.getTime()) {
    return "MISSED";
  }

  return "PENDING";
}


// =====================================================
// FETCH ESP32 STATUS
// =====================================================

async function fetchDevice() {
  try {
    const controller = new AbortController();

    const timeout = setTimeout(() => {
      controller.abort();
    }, 2000);

    const response = await fetch(
      `${ESP32_URL}/api/status?_=${Date.now()}`,
      {
        method: "GET",
        signal: controller.signal,
        cache: "no-store"
      }
    );

    clearTimeout(timeout);

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const data = await response.json();

    lastDevice = {
      ...data,
      online: true,
      device: data.device || "ESP32-001",
      ip: data.ip || getESP32IP(),
      lastSeen: new Date().toISOString()
    };

    // -------------------------------------------------
    // Sync schedule from ESP32
    // -------------------------------------------------

    if (Array.isArray(data.chambers)) {
      data.chambers.forEach(chamber => {
        const index = chamber.chamber - 1;

        if (
          index >= 0 &&
          index < schedule.length &&
          chamber.schedule
        ) {
          schedule[index].time = chamber.schedule;
        }
      });
    }

    io.emit(
      "device:update",
      buildDashboardState()
    );

  } catch (err) {

    lastDevice = {
      ...lastDevice,
      online: false,
      ip: getESP32IP()
    };

    io.emit(
      "device:update",
      buildDashboardState()
    );
  }
}


// =====================================================
// DASHBOARD STATE
// =====================================================

function buildDashboardState() {

  const now = new Date();

  const doses = schedule.map(item => ({
    ...item,
    status: statusForDose(item, now)
  }));

  const taken =
    doses.filter(x => x.status === "TAKEN").length;

  const missed =
    doses.filter(x => x.status === "MISSED").length;

  const remaining =
    doses.filter(x => x.status === "PENDING").length;

  const adherence =
    doses.length
      ? Math.round((taken / doses.length) * 100)
      : 0;

  return {
    patient: {
      name: "Patient Name",
      id: "AMD-001",
      age: "—"
    },

    device: lastDevice,

    schedule: doses,

    summary: {
      total: doses.length,
      taken,
      missed,
      remaining,
      adherence
    },

    serverTime: now.toISOString()
  };
}


// =====================================================
// DASHBOARD STATE API
// =====================================================

app.get("/api/state", (req, res) => {
  res.set("Cache-Control", "no-store");

  res.json(
    buildDashboardState()
  );
});


// =====================================================
// ESP32 CONFIG API
// =====================================================

app.get("/api/config", (req, res) => {

  res.json({
    esp32Url: ESP32_URL,
    esp32Ip: getESP32IP()
  });

});


app.post("/api/config", async (req, res) => {

  const newURL =
    normalizeESP32URL(
      req.body?.esp32Url
    );

  if (!newURL) {

    return res.status(400).json({
      ok: false,
      error: "Missing esp32Url"
    });

  }

  ESP32_URL = newURL;

  lastDevice.ip =
    getESP32IP();

  await fetchDevice();

  res.json({
    ok: true,
    esp32Url: ESP32_URL,
    esp32Ip: getESP32IP()
  });

});


// =====================================================
// UPDATE COMPLETE SCHEDULE
// =====================================================

app.post("/api/schedule", async (req, res) => {

  const newSchedule = req.body;

  if (!Array.isArray(newSchedule)) {

    return res.status(400).json({
      ok: false,
      error: "Schedule must be an array"
    });

  }

  if (newSchedule.length !== 4) {

    return res.status(400).json({
      ok: false,
      error: "Exactly 4 chambers are required"
    });

  }

  schedule = newSchedule;

  // Try to update ESP32 schedules
  for (const item of schedule) {

    try {

      if (
        item.chamber >= 1 &&
        item.chamber <= 4 &&
        /^\d{2}:\d{2}$/.test(item.time)
      ) {

        await fetch(
          `${ESP32_URL}/api/set?chamber=${item.chamber}&time=${encodeURIComponent(item.time)}`,
          {
            method: "GET"
          }
        );

      }

    } catch (err) {

      console.log(
        `Could not update CH${item.chamber} on ESP32`
      );

    }
  }

  io.emit(
    "device:update",
    buildDashboardState()
  );

  res.json({
    ok: true,
    schedule
  });

});


// =====================================================
// UPDATE ONE CHAMBER SCHEDULE
// =====================================================

app.post("/api/schedule/:chamber", async (req, res) => {

  const chamber =
    Number(req.params.chamber);

  const time =
    req.body?.time;

  if (
    ![1, 2, 3, 4].includes(chamber)
  ) {

    return res.status(400).json({
      ok: false,
      error: "Chamber must be 1-4"
    });

  }

  if (
    typeof time !== "string" ||
    !/^\d{2}:\d{2}$/.test(time)
  ) {

    return res.status(400).json({
      ok: false,
      error: "Time must be HH:MM"
    });

  }

  const [hour, minute] =
    time.split(":").map(Number);

  if (
    hour > 23 ||
    minute > 59
  ) {

    return res.status(400).json({
      ok: false,
      error: "Invalid time"
    });

  }

  try {

    const response = await fetch(
      `${ESP32_URL}/api/set?chamber=${chamber}&time=${encodeURIComponent(time)}`
    );

    const text =
      await response.text();

    if (!response.ok) {
      throw new Error(text);
    }

    schedule[chamber - 1].time =
      time;

    io.emit(
      "device:update",
      buildDashboardState()
    );

    res.json({
      ok: true,
      chamber,
      time,
      esp32: text
    });

  } catch (err) {

    res.status(502).json({
      ok: false,
      error: "Could not update ESP32 schedule",
      details: err.message
    });

  }

});


// =====================================================
// MANUAL DISPENSE
// =====================================================

app.post(
  "/api/dispense/:chamber",
  async (req, res) => {

    const chamber =
      Number(req.params.chamber);

    if (
      ![1, 2, 3, 4].includes(chamber)
    ) {

      return res.status(400).json({
        ok: false,
        error: "Chamber must be 1-4"
      });

    }

    try {

      const response = await fetch(
        `${ESP32_URL}/dispense?chamber=${chamber}`,
        {
          method: "POST"
        }
      );

      const data =
        await response.json();

      if (!response.ok) {

        throw new Error(
          data.error ||
          "ESP32 command failed"
        );

      }

      takenToday.add(chamber);

      io.emit(
        "device:update",
        buildDashboardState()
      );

      res.json({
        ok: true,
        esp32: data
      });

    } catch (err) {

      res.status(502).json({
        ok: false,
        error: "ESP32 is not reachable",
        details: err.message
      });

    }

  }
);


// =====================================================
// MANUAL OPEN
// =====================================================

app.post(
  "/api/open/:chamber",
  async (req, res) => {

    const chamber =
      Number(req.params.chamber);

    if (
      ![1, 2, 3, 4].includes(chamber)
    ) {

      return res.status(400).json({
        ok: false,
        error: "Chamber must be 1-4"
      });

    }

    try {

      const response = await fetch(
        `${ESP32_URL}/api/open?chamber=${chamber}`
      );

      const text =
        await response.text();

      if (!response.ok) {
        throw new Error(text);
      }

      io.emit(
        "device:update",
        buildDashboardState()
      );

      res.json({
        ok: true,
        message: text
      });

    } catch (err) {

      res.status(502).json({
        ok: false,
        error: "ESP32 is not reachable",
        details: err.message
      });

    }

  }
);


// =====================================================
// SET ESP32 RTC TIME
// =====================================================

app.post("/api/rtc", async (req, res) => {

  const time =
    req.body?.time;

  if (
    typeof time !== "string" ||
    !/^([01]\d|2[0-3]):[0-5]\d(:[0-5]\d)?$/.test(time)
  ) {

    return res.status(400).json({
      ok: false,
      error: "Time must be HH:MM or HH:MM:SS"
    });

  }

  try {

    const response = await fetch(
      `${ESP32_URL}/api/rtc-set?time=${encodeURIComponent(time)}`
    );

    const text =
      await response.text();

    if (!response.ok) {
      throw new Error(text);
    }

    io.emit(
      "device:update",
      buildDashboardState()
    );

    res.json({
      ok: true,
      message: text
    });

  } catch (err) {

    res.status(502).json({
      ok: false,
      error: "Could not set ESP32 RTC",
      details: err.message
    });

  }

});


// =====================================================
// NTP SYNC
// =====================================================

app.post("/api/rtc-sync", async (req, res) => {

  try {

    const response =
      await fetch(
        `${ESP32_URL}/api/rtc-sync`
      );

    const text =
      await response.text();

    if (!response.ok) {
      throw new Error(text);
    }

    io.emit(
      "device:update",
      buildDashboardState()
    );

    res.json({
      ok: true,
      message: text
    });

  } catch (err) {

    res.status(502).json({
      ok: false,
      error: "NTP sync failed",
      details: err.message
    });

  }

});


// =====================================================
// RESET TODAY
// =====================================================

app.post("/api/reset-today", (req, res) => {

  takenToday.clear();

  io.emit(
    "device:update",
    buildDashboardState()
  );

  res.json({
    ok: true
  });

});


// =====================================================
// SOCKET.IO
// =====================================================

io.on("connection", socket => {

  console.log(
    "Dashboard connected:",
    socket.id
  );

  socket.emit(
    "device:update",
    buildDashboardState()
  );

});


// =====================================================
// ESP32 POLLING
// =====================================================

setInterval(
  fetchDevice,
  1000
);


// =====================================================
// INITIAL FETCH
// =====================================================

fetchDevice();


// =====================================================
// START SERVER
// =====================================================

server.listen(
  PORT,
  () => {

    console.log(
      "===================================="
    );

    console.log(
      `MEDINOSA Dashboard: http://localhost:${PORT}`
    );

    console.log(
      `ESP32 URL: ${ESP32_URL}`
    );

    console.log(
      "===================================="
    );

  }
);