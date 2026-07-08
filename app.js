// ============================================================
// FIREBASE CONFIGURATION
// ============================================================
const firebaseConfig = {
    apiKey: "AIzaSyA51PFwiu3aNdL4AiZHaX7bxO3925w1iDI",
    authDomain: "crowd-counter-uu.firebaseapp.com",
    databaseURL: "https://crowd-counter-uu-default-rtdb.europe-west1.firebasedatabase.app",
    projectId: "crowd-counter-uu",
    storageBucket: "crowd-counter-uu.firebasestorage.app",
    messagingSenderId: "726827199534",
    appId: "1:726827199534:web:1a704b19c9ac8407b843bf"
};

// Initialize Firebase
firebase.initializeApp(firebaseConfig);
const db = firebase.database();

// ============================================================
// DOM Elements
// ============================================================
const currentCountEl = document.getElementById('current-count');
const totalInEl = document.getElementById('total-in');
const totalOutEl = document.getElementById('total-out');
const co2ValueEl = document.getElementById('co2-value');
const noiseValueEl = document.getElementById('noise-value');
const activityLog = document.getElementById('activity-log');
const statusEl = document.getElementById('status');
const statusText = document.getElementById('status-text');

// ============================================================
// Listen for real-time changes to the counters
// ============================================================
const counterRef = db.ref('crowd_counter');

counterRef.on('value', (snapshot) => {
    const data = snapshot.val();
    if (data) {
        currentCountEl.textContent = data.current_count || 0;
        totalInEl.textContent = data.total_in || 0;
        totalOutEl.textContent = data.total_out || 0;
    }
    // Mark as connected
    statusEl.className = 'status connected';
    statusText.textContent = 'Connected — Live';
}, (error) => {
    console.error('Firebase error:', error);
    statusEl.className = 'status error';
    statusText.textContent = 'Connection error';
});

// ============================================================
// Listen for CO2 sensor data
// ============================================================
const co2Ref = db.ref('sensors/co2');

co2Ref.on('value', (snapshot) => {
    const data = snapshot.val();
    if (data) {
        co2ValueEl.textContent = data.current_ppm || '—';
    }
});

// ============================================================
// Listen for Noise sensor data
// ============================================================
const noiseRef = db.ref('sensors/noise');

noiseRef.on('value', (snapshot) => {
    const data = snapshot.val();
    if (data) {
        noiseValueEl.textContent = data.current_db || '—';
    }
});

// ============================================================
// Listen for new events in the activity log
// ============================================================
const eventsRef = db.ref('crowd_counter/events');

// Time formatting options — Netherlands timezone
const timeOptions = {
    timeZone: 'Europe/Amsterdam',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false
};

const dateTimeOptions = {
    timeZone: 'Europe/Amsterdam',
    day: '2-digit',
    month: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false
};

// Helper: create a log entry element
function createLogEntry(event) {
    const entry = document.createElement('div');
    entry.className = 'log-entry';

    const badge = document.createElement('span');
    const text = document.createElement('span');

    if (event.type === 'in' || event.type === 'out') {
        badge.className = 'log-badge ' + event.type;
        badge.textContent = event.type === 'in' ? 'IN' : 'OUT';
        text.textContent = event.type === 'in' ? 'Person entered' : 'Person exited';
    } else if (event.type === 'co2') {
        badge.className = 'log-badge co2';
        badge.textContent = 'CO₂';
        text.textContent = event.ppm + ' ppm';
    } else if (event.type === 'noise') {
        badge.className = 'log-badge noise';
        badge.textContent = 'dB';
        text.textContent = event.db + ' dB';
    }

    const time = document.createElement('span');
    time.className = 'log-time';
    const date = new Date(event.timestamp);
    time.textContent = date.toLocaleTimeString('nl-NL', timeOptions);

    entry.appendChild(badge);
    entry.appendChild(text);
    entry.appendChild(time);
    return entry;
}

// Load last 20 events and listen for new ones
eventsRef.orderByChild('timestamp').limitToLast(20).on('value', (snapshot) => {
    const data = snapshot.val();

    // Clear the log
    activityLog.innerHTML = '';

    if (!data) {
        activityLog.innerHTML = '<p class="log-empty">No events yet. Waiting for data...</p>';
        return;
    }

    // Convert to array and sort newest first
    const events = Object.values(data).sort((a, b) => b.timestamp - a.timestamp);

    events.forEach((event) => {
        activityLog.appendChild(createLogEntry(event));
    });
});

// ============================================================
// Occupancy Over Time Chart
// ============================================================
const chartCtx = document.getElementById('occupancy-chart').getContext('2d');

const occupancyChart = new Chart(chartCtx, {
    type: 'bar',
    data: {
        labels: [],
        datasets: [{
            label: 'People Inside',
            data: [],
            backgroundColor: [],
            borderRadius: 4,
            borderSkipped: false,
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: true,
        aspectRatio: 2.5,
        plugins: {
            legend: { display: false },
            tooltip: {
                backgroundColor: '#16213e',
                titleColor: '#fff',
                bodyColor: '#ccc',
                borderColor: '#2a2a4a',
                borderWidth: 1,
                callbacks: {
                    title: (items) => items[0].label,
                    label: (item) => `  ${item.raw} people`
                }
            }
        },
        scales: {
            x: {
                grid: { display: false },
                ticks: { color: '#888', font: { size: 11 } }
            },
            y: {
                beginAtZero: true,
                grid: { color: 'rgba(255,255,255,0.05)' },
                ticks: {
                    color: '#888',
                    stepSize: 1,
                    font: { size: 11 }
                }
            }
        }
    }
});

// Listen to recent events to build the chart
const allEventsRef = db.ref('crowd_counter/events');

allEventsRef.orderByChild('timestamp').limitToLast(500).on('value', (snapshot) => {
    const data = snapshot.val();
    if (!data) return;

    // Convert to sorted array (oldest first)
    const events = Object.values(data).sort((a, b) => a.timestamp - b.timestamp);
    if (events.length === 0) return;

    // Get the actual current count from the counter card
    const actualCount = parseInt(currentCountEl.textContent) || 0;

    const now = new Date();

    // Group events by the minute they occurred — ONLY count people events
    const eventsByMinute = {};
    events.forEach((event) => {
        if (event.type !== 'in' && event.type !== 'out') return; // skip sensor events

        const d = new Date(event.timestamp);
        d.setSeconds(0, 0); // truncate to minute
        const t = d.getTime();
        if (!eventsByMinute[t]) eventsByMinute[t] = 0;
        
        // +1 if someone walked in, -1 if out
        eventsByMinute[t] += (event.type === 'in') ? 1 : -1;
    });

    const buckets = {};
    let count = actualCount;

    // Generate buckets going BACKWARDS from the current minute down to 9 minutes ago
    for (let i = 0; i <= 9; i++) {
        const d = new Date(now.getTime() - i * 60000);
        d.setSeconds(0, 0);
        const t = d.getTime();

        const key = d.toLocaleTimeString('nl-NL', {
            timeZone: 'Europe/Amsterdam',
            hour: '2-digit',
            minute: '2-digit',
            hour12: false
        });

        // The count for this minute bucket is the count BEFORE we undo this minute's events
        buckets[key] = count;

        // Now undo this minute's events to find the count for the previous minute
        if (eventsByMinute[t]) {
            count -= eventsByMinute[t];
            if (count < 0) count = 0; // sanity check
        }
    }

    // Since we went backwards, reverse the buckets to be chronological (oldest to newest)
    const labels = Object.keys(buckets).reverse();
    const values = Object.values(buckets).reverse();

    // Color: light blue for past, bright blue for the latest (current)
    const colors = values.map((_, i) =>
        i === values.length - 1
            ? 'rgba(52, 152, 219, 1.0)'    // current — bright blue
            : 'rgba(52, 152, 219, 0.4)'     // past — faded blue
    );

    // Update chart
    occupancyChart.data.labels = labels;
    occupancyChart.data.datasets[0].data = values;
    occupancyChart.data.datasets[0].backgroundColor = colors;
    occupancyChart.update();
});

// ============================================================
// CO2 Over Time Chart
// ============================================================
const co2Ctx = document.getElementById('co2-chart').getContext('2d');

// Create gradient for CO2 chart
const co2Gradient = co2Ctx.createLinearGradient(0, 0, 0, 250);
co2Gradient.addColorStop(0, 'rgba(243, 156, 18, 0.4)');
co2Gradient.addColorStop(1, 'rgba(243, 156, 18, 0.02)');

const co2Chart = new Chart(co2Ctx, {
    type: 'line',
    data: {
        labels: [],
        datasets: [{
            label: 'CO₂ (ppm)',
            data: [],
            borderColor: '#f39c12',
            backgroundColor: co2Gradient,
            borderWidth: 2,
            fill: true,
            tension: 0.6,          
            pointRadius: 0,        
            pointHoverRadius: 5,   
            pointBackgroundColor: '#f39c12',
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: true,
        aspectRatio: 1.8,
        plugins: {
            legend: { display: false },
            tooltip: {
                backgroundColor: '#16213e',
                titleColor: '#fff',
                bodyColor: '#ccc',
                borderColor: '#2a2a4a',
                borderWidth: 1,
                callbacks: {
                    label: (item) => `  ${item.raw} ppm`
                }
            }
        },
        scales: {
            x: {
                grid: { display: false },
                ticks: { color: '#888', font: { size: 10 }, maxTicksLimit: 8 }
            },
            y: {
                grid: { color: 'rgba(255,255,255,0.05)' },
                ticks: { color: '#888', font: { size: 10 } }
            }
        }
    }
});

// Listen for CO2 readings
const co2ReadingsRef = db.ref('sensors/co2/readings');

co2ReadingsRef.orderByChild('timestamp').limitToLast(120).on('value', (snapshot) => {
    const data = snapshot.val();
    if (!data) return;

    const readings = Object.values(data).sort((a, b) => a.timestamp - b.timestamp);

    // Limit to exactly the last 10 minutes
    const now = new Date();
    const tenMinsAgo = new Date(now.getTime() - 10 * 60000);
    const recentReadings = readings.filter(r => r.timestamp >= tenMinsAgo.getTime());

    const labels = recentReadings.map(r => {
        const d = new Date(r.timestamp);
        return d.toLocaleTimeString('nl-NL', timeOptions);
    });
    const values = recentReadings.map(r => r.ppm);

    co2Chart.data.labels = labels;
    co2Chart.data.datasets[0].data = values;
    co2Chart.update();
});

// ============================================================
// Noise Over Time Chart
// ============================================================
const noiseCtx = document.getElementById('noise-chart').getContext('2d');

// Create gradient for Noise chart
const noiseGradient = noiseCtx.createLinearGradient(0, 0, 0, 250);
noiseGradient.addColorStop(0, 'rgba(155, 89, 182, 0.4)');
noiseGradient.addColorStop(1, 'rgba(155, 89, 182, 0.02)');

const noiseChart = new Chart(noiseCtx, {
    type: 'line',
    data: {
        labels: [],
        datasets: [{
            label: 'Noise (dB)',
            data: [],
            borderColor: '#9b59b6',
            backgroundColor: noiseGradient,
            borderWidth: 2,
            fill: true,
            tension: 0.6,          // Increased tension for smoother curve
            pointRadius: 0,        // Hide points on the line
            pointHoverRadius: 5,   // Still show points on hover
            pointBackgroundColor: '#9b59b6',
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: true,
        aspectRatio: 1.8,
        plugins: {
            legend: { display: false },
            tooltip: {
                backgroundColor: '#16213e',
                titleColor: '#fff',
                bodyColor: '#ccc',
                borderColor: '#2a2a4a',
                borderWidth: 1,
                callbacks: {
                    label: (item) => `  ${item.raw} dB`
                }
            }
        },
        scales: {
            x: {
                grid: { display: false },
                ticks: { color: '#888', font: { size: 10 }, maxTicksLimit: 8 }
            },
            y: {
                grid: { color: 'rgba(255,255,255,0.05)' },
                ticks: { color: '#888', font: { size: 10 } }
            }
        }
    }
});

// Listen for Noise readings
const noiseReadingsRef = db.ref('sensors/noise/readings');

noiseReadingsRef.orderByChild('timestamp').limitToLast(120).on('value', (snapshot) => {
    const data = snapshot.val();
    if (!data) return;

    const readings = Object.values(data).sort((a, b) => a.timestamp - b.timestamp);

    // Limit to exactly the last 10 minutes
    const now = new Date();
    const tenMinsAgo = new Date(now.getTime() - 10 * 60000);
    const recentReadings = readings.filter(r => r.timestamp >= tenMinsAgo.getTime());

    const labels = recentReadings.map(r => {
        const d = new Date(r.timestamp);
        return d.toLocaleTimeString('nl-NL', timeOptions);
    });
    const values = recentReadings.map(r => r.db);

    noiseChart.data.labels = labels;
    noiseChart.data.datasets[0].data = values;
    noiseChart.update();
});

