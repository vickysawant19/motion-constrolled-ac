const express = require("express");
const cors = require("cors");
const http = require("http");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);

const io = new Server(server, {
  pingInterval: 10000,
  pingTimeout: 60000,
  cors: {
    origin: "*",
    methods: ["GET", "POST"],
    transports: ["websocket", "polling"],
    credentials: true,
  },
  allowEIO3: true,
});

app.use(cors());
app.use(express.static("public"));
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// Constants
const ACTIONS = {
  TURN_ON: "turnOn",
  TURN_OFF: "turnOff",
  GET_STATUS: "status",
};

const DEVICE_TIMEOUT = 60000; // 1 minute timeout for devices

// State management
const devices = new Map(); // {chipId: {socketId, lastSeen, status}}
const clients = new Map(); // {socketId: {chipIds}}

// Utility functions
function updateDeviceStatus(chipId, status) {
  if (devices.has(chipId)) {
    const device = devices.get(chipId);
    devices.set(chipId, { ...device, ...status, lastSeen: Date.now() });
    return true;
  }
  return false;
}

function removeStaleDevices() {
  const now = Date.now();
  for (const [chipId, device] of devices.entries()) {
    if (now - device.lastSeen > DEVICE_TIMEOUT) {
      console.log(`Removing stale device: ${chipId}`);
      devices.delete(chipId);
      notifyClientsOfDeviceDisconnection(chipId);
    }
  }
}

function notifyClientsOfDeviceDisconnection(chipId) {
  for (const [clientSocketId, { chipIds }] of clients.entries()) {
    if (chipIds.includes(chipId)) {
      io.to(clientSocketId).emit("deviceDisconnected", { chipId });
    }
  }
}

// Set up device cleanup interval
setInterval(removeStaleDevices, 30000);

io.on("connection", (socket) => {
  console.log(`New connection: ${socket.id}`);

  // Device registration
  socket.on("deviceRegister", ({ chipId }) => {
    if (!chipId) {
      return socket.emit("error", { message: "chipId is required" });
    }
    if (devices.get(chipId)) {
      console.log(`Device Already registered: ${chipId}`);
      socket.emit("registerConfirm", { success: true, chipId });
    }

    devices.set(chipId, {
      socketId: socket.id,
      lastSeen: Date.now(),
      relayStatus: false,
      pirStatus: false,
    });

    console.log(`Device registered: ${chipId}`);
    socket.emit("registerConfirm", { success: true, chipId });
  });

  // Client registration
  socket.on("clientRegister", ({ chipIds }) => {
    if (!Array.isArray(chipIds)) {
      return socket.emit("error", { message: "chipIds must be an array" });
    }

    clients.set(socket.id, { chipIds });

    // Send initial status for all registered devices
    const statuses = chipIds
      .filter((chipId) => devices.has(chipId))
      .map((chipId) => ({
        chipId,
        ...devices.get(chipId),
        socketId: undefined, // Don't send internal socketId to clients
      }));

    socket.emit("registerConfirm", { success: true, devices: statuses });
  });

  // Handle sensor requests from clients
  socket.on("sensorRequest", ({ chipId, action }) => {
    if (!devices.has(chipId)) {
      return socket.emit("sensorResponse", {
        chipId,
        success: false,
        error: "Device not found",
      });
    }

    if (!Object.values(ACTIONS).includes(action)) {
      return socket.emit("sensorResponse", {
        chipId,
        success: false,
        error: "Invalid action",
      });
    }

    const device = devices.get(chipId);
    io.to(device.socketId).emit("sensorRequest", { action });
  });

  // Handle sensor responses from devices
  socket.on("sensorResponse", ({ chipId, relayStatus, pirStatus }) => {
    if (!devices.has(chipId)) {
      return socket.emit("error", { message: "Device not registered" });
    }

    if (updateDeviceStatus(chipId, { relayStatus, pirStatus })) {
      // Notify relevant clients
      for (const [clientSocketId, { chipIds }] of clients.entries()) {
        if (chipIds.includes(chipId)) {
          io.to(clientSocketId).emit("sensorResponse", {
            chipId,
            relayStatus,
            pirStatus,
            success: true,
          });
        }
      }
    }
  });

  // Handle heartbeat from devices
  socket.on("heartbeat", ({ chipId }) => {
    if (devices.has(chipId)) {
      const device = devices.get(chipId);
      device.lastSeen = Date.now();
      devices.set(chipId, device);
    }
  });

  // Handle disconnections
  socket.on("disconnect", () => {
    console.log(`Disconnected: ${socket.id}`);

    // Handle client disconnection
    if (clients.has(socket.id)) {
      clients.delete(socket.id);
    }

    // Handle device disconnection
    for (const [chipId, device] of devices.entries()) {
      if (device.socketId === socket.id) {
        devices.delete(chipId);
        notifyClientsOfDeviceDisconnection(chipId);
      }
    }
  });
});

// Error handling middleware
app.use((err, req, res, next) => {
  console.error(err.stack);
  res.status(500).send("Something broke!");
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
});
