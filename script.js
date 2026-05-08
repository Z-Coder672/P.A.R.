function bitmapBase64ToCanvas(base64) {
    const raw = atob(base64);
    const canvas = document.createElement('canvas');
    canvas.width = 37;
    canvas.height = 18;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, 37, 18);
    ctx.fillStyle = '#02b2d9';
    for (let i = 0; i < 666; i++) {
        if ((raw.charCodeAt(Math.floor(i / 8)) >> (7 - (i % 8))) & 1) {
            ctx.fillRect(i % 37, Math.floor(i / 37), 1, 1);
        }
    }
    return canvas;
}

// Tab switching with URL routing
const navLinks = document.querySelectorAll('.nav-link');
const tabContents = document.querySelectorAll('.tab-content');

// Function to show a specific tab
function showTab(tabName) {
    tabContents.forEach(tab => {
        tab.classList.remove('active');
    });

    navLinks.forEach(l => {
        l.classList.remove('active');
    });

    const selectedTab = document.getElementById(tabName);
    if (selectedTab) {
        selectedTab.classList.add('active');
    }

    document.body.classList.toggle('upload-tab-active', tabName === 'upload');

    const activeLink = document.querySelector(`[data-tab="${tabName}"]`);
    if (activeLink) {
        activeLink.classList.add('active');
    }

    updateUploadRotatePrompt();

    if (tabName === 'gallery') {
        loadGallery();
        startGalleryPolling();
    } else {
        stopGalleryPolling();
    }
}

const GALLERY_POLL_INTERVAL_MS = 5000;
let galleryPollTimer = null;

function startGalleryPolling() {
    stopGalleryPolling();
    galleryPollTimer = setInterval(() => {
        loadGallery({ silent: true });
    }, GALLERY_POLL_INTERVAL_MS);
}

function stopGalleryPolling() {
    if (galleryPollTimer !== null) {
        clearInterval(galleryPollTimer);
        galleryPollTimer = null;
    }
}

// Determine active tab from URL
function getActiveTabFromUrl() {
    const pathname = window.location.pathname.replace(/\/$/, '');
    if (pathname === '/upload') return 'upload';
    if (pathname === '/gallery') return 'gallery';
    if (pathname === '/about') return 'about';
    return 'livestream';
}

// Livestream search and embed functionality
async function loadPARLivestream() {
    const livestreamContainer = document.querySelector('.livestream-container');

    try {
        livestreamContainer.innerHTML = '';
        const loadingBox = document.createElement('div');
        loadingBox.className = 'livestream-loading-box';
        loadingBox.textContent = 'Loading livestream...';
        livestreamContainer.appendChild(loadingBox);

        const response = await fetch('/livestream-api.php');
        const data = await response.json();

        if (data.found && data.videoId) {
            if (livestreamContainer) {
                const iframeWrapper = document.createElement('div');
                iframeWrapper.className = 'livestream-iframe-wrapper';
                iframeWrapper.style.display = 'none';

                const iframe = document.createElement('iframe');
                iframe.width = '100%';
                iframe.height = '100%';
                iframe.src = `https://www.youtube.com/embed/${data.videoId}?autoplay=1`;
                iframe.frameBorder = '0';
                iframe.allow = 'accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture';
                iframe.allowFullscreen = true;
                iframe.className = 'livestream-iframe';

                iframe.onload = function() {
                    loadingBox.style.display = 'none';
                    iframeWrapper.style.display = 'block';
                };

                iframeWrapper.appendChild(iframe);
                livestreamContainer.appendChild(iframeWrapper);

                const title = document.createElement('p');
                title.className = 'livestream-caption';
                title.textContent = data.title;
                livestreamContainer.appendChild(title);
            }
        } else {
            if (livestreamContainer) {
                livestreamContainer.innerHTML = '';
                const noLiveBox = document.createElement('div');
                noLiveBox.className = 'no-livestream-box';
                noLiveBox.textContent = 'No livestream available';
                livestreamContainer.appendChild(noLiveBox);
            }
        }
    } catch (error) {
        console.error('Error loading livestream:', error);
        if (livestreamContainer) {
            livestreamContainer.innerHTML = '';
            const errorBox = document.createElement('div');
            errorBox.className = 'no-livestream-box';
            errorBox.textContent = 'Error loading livestream';
            livestreamContainer.appendChild(errorBox);
        }
    }
}

// Gallery
async function loadGallery({ silent = false } = {}) {
    const container = document.getElementById('galleryContainer');
    if (!container) return;

    if (!silent) {
        container.innerHTML = '<p class="gallery-loading">Loading...</p>';
    }

    try {
        const response = await fetch('/gallery.php');
        const data = await response.json();

        const items = data.items || [];

        if (items.length === 0) {
            container.innerHTML = '';
            const placeholder = document.createElement('div');
            placeholder.className = 'gallery-placeholder';
            placeholder.innerHTML = `
                <div class="gallery-placeholder-icon">
                    <i class="fa-solid fa-image"></i>
                </div>
                <p class="gallery-placeholder-text">No pictures yet</p>
                <p class="gallery-placeholder-sub">Pictures appear here after being displayed on the P.A.R.</p>
            `;
            container.appendChild(placeholder);
            return;
        }

        const grid = document.createElement('div');
        grid.className = 'gallery-grid';

        items.forEach(item => {
            const card = document.createElement('div');
            card.className = 'gallery-card';

            const img = bitmapBase64ToCanvas(item.bitmap);
            img.className = 'gallery-image';

            card.appendChild(img);
            card.addEventListener('click', () => openGalleryItem(item));

            grid.appendChild(card);
        });

        container.innerHTML = '';
        container.appendChild(grid);
    } catch (error) {
        console.error('Error loading gallery:', error);
        if (!silent) {
            container.innerHTML = '<p class="gallery-loading">Error loading gallery</p>';
        }
    }
}

// Gallery item modal — re-fetches gallery state so a stale cached `item.pending`
// from the initial render can't make a completed entry look "In progress".
async function openGalleryItem(cachedItem) {
    let item = cachedItem;
    try {
        const response = await fetch('/gallery.php');
        const data = await response.json();
        const fresh = (data.items || []).find(it => it.id === cachedItem.id);
        if (fresh) item = fresh;
    } catch (error) {
        console.error('Error refreshing gallery item:', error);
    }
    openGalleryModal(item);
}

function openGalleryModal(item) {
    const modal = document.getElementById('galleryItemModal');
    const nameEl = document.getElementById('galleryModalName');
    const imgEl = document.getElementById('galleryModalImage');
    const snapshotEl = document.getElementById('galleryModalSnapshot');
    const noSnapshotEl = document.getElementById('galleryModalNoSnapshot');
    const pendingEl = document.getElementById('galleryModalPending');

    nameEl.textContent = '';
    imgEl.classList.add('hidden');
    snapshotEl.classList.add('hidden');
    snapshotEl.removeAttribute('src');
    noSnapshotEl.classList.add('hidden');
    pendingEl.classList.add('hidden');

    nameEl.textContent = item.name || '(unnamed)';

    if (item.bitmap) {
        const src = bitmapBase64ToCanvas(item.bitmap);
        imgEl.width = src.width;
        imgEl.height = src.height;
        imgEl.getContext('2d').drawImage(src, 0, 0);
        imgEl.classList.remove('hidden');
    }

    if (item.pending) {
        pendingEl.classList.remove('hidden');
    } else if (item.image) {
        snapshotEl.src = item.image;
        snapshotEl.classList.remove('hidden');
    } else {
        noSnapshotEl.classList.remove('hidden');
    }

    modal.classList.remove('hidden');
}

function closeGalleryModal() {
    document.getElementById('galleryItemModal').classList.add('hidden');
}

// Load livestream when page is ready
document.addEventListener('DOMContentLoaded', function() {
    loadPARLivestream();

    document.getElementById('closeGalleryModal').addEventListener('click', closeGalleryModal);
    document.getElementById('galleryItemModal').addEventListener('click', function(e) {
        if (e.target === this) closeGalleryModal();
    });
    document.getElementById('galleryModalLivestreamLink').addEventListener('click', function(e) {
        e.preventDefault();
        closeGalleryModal();
        window.history.pushState({tab: 'livestream'}, '', '/livestream');
        showTab('livestream');
    });
});

// Handle navigation clicks
navLinks.forEach(link => {
    link.addEventListener('click', function(e) {
        e.preventDefault();

        const tabName = this.getAttribute('data-tab');
        const url = `/${tabName}`;

        window.history.pushState({tab: tabName}, '', url);
        showTab(tabName);
    });
});

// Handle browser back/forward buttons
window.addEventListener('popstate', function(e) {
    const tabName = getActiveTabFromUrl();
    showTab(tabName);
});

// Get elements
const uploadBtn = document.getElementById('uploadBtn');
const sendQueueBtn = document.getElementById('sendQueueBtn');
const fileInput = document.getElementById('fileInput');
const pixelGrid = document.getElementById('pixelGrid');
const clearPixelsBtn = document.getElementById('clearPixels');
const uploadRotatePrompt = document.getElementById('uploadRotatePrompt');
const queueStatus = document.getElementById('queueStatus');

// Pixel Art Maker - Create grid
const GRID_WIDTH = 37;
const GRID_HEIGHT = 18;
const THEME_COLOR = '#02b2d9';
const BLACK = '#000000';
const GRID_STORAGE_KEY = 'pixelGridState';

function isMobileUploadPortrait() {
    return window.matchMedia('(max-width: 768px) and (orientation: portrait)').matches;
}

function updateUploadRotatePrompt() {
    if (!uploadRotatePrompt) {
        return;
    }

    const uploadTabIsActive = document.getElementById('upload')?.classList.contains('active');
    const shouldShowPrompt = uploadTabIsActive && isMobileUploadPortrait();

    uploadRotatePrompt.classList.toggle('hidden', !shouldShowPrompt);
    uploadRotatePrompt.setAttribute('aria-hidden', shouldShowPrompt ? 'false' : 'true');
}

// Set initial active tab based on current URL
showTab(getActiveTabFromUrl());

// Undo/Redo history management
const HISTORY_BUFFER_SIZE = 100;
let drawingHistory = [];
let historyIndex = -1;

function captureGridState() {
    const pixels = document.querySelectorAll('.pixel');
    return Array.from(pixels).map(pixel => pixel.classList.contains('active'));
}

function encodeGridStateToBase64(state) {
    const bytes = [];

    for (let i = 0; i < state.length; i += 8) {
        let byte = 0;
        for (let bit = 0; bit < 8; bit++) {
            byte <<= 1;
            byte |= state[i + bit] ? 1 : 0;
        }
        bytes.push(byte);
    }

    return btoa(String.fromCharCode(...bytes));
}

function setQueueStatus(message, isError = false) {
    if (!queueStatus) {
        return;
    }

    queueStatus.textContent = message;
    queueStatus.classList.toggle('error', isError);
}

function getQueueErrorMessage(errorCode, statusCode) {
    if (statusCode === 409 && errorCode === 'duplicate_queue_item') {
        return 'That picture is already in the P.A.R. queue.';
    }

    if (statusCode === 409 && errorCode === 'queue_full') {
        return 'The P.A.R. queue is full. Try again later.';
    }

    if (statusCode === 400 && errorCode === 'Invalid bitmap payload') {
        return 'This picture could not be prepared for the queue.';
    }

    return 'Could not add picture to the P.A.R. queue.';
}

function persistGridState(state = captureGridState()) {
    try {
        localStorage.setItem(GRID_STORAGE_KEY, JSON.stringify(state));
    } catch (error) {
        console.error('Error saving grid state:', error);
    }
}

function loadSavedGridState() {
    try {
        const savedState = localStorage.getItem(GRID_STORAGE_KEY);
        if (!savedState) {
            return null;
        }

        const parsedState = JSON.parse(savedState);
        if (!Array.isArray(parsedState) || parsedState.length !== GRID_WIDTH * GRID_HEIGHT) {
            return null;
        }

        return parsedState.map(Boolean);
    } catch (error) {
        console.error('Error loading grid state:', error);
        return null;
    }
}

function restoreGridFromState(state) {
    const pixels = document.querySelectorAll('.pixel');
    pixels.forEach((pixel, index) => {
        if (state[index]) {
            pixel.classList.add('active');
        } else {
            pixel.classList.remove('active');
        }
    });
}

function renderPreview(state, elementId) {
    const previewGrid = document.getElementById(elementId);
    if (!previewGrid) {
        return;
    }

    previewGrid.innerHTML = '';

    state.forEach(isActive => {
        const pixel = document.createElement('div');
        pixel.className = 'preview-pixel';
        pixel.style.backgroundColor = isActive ? THEME_COLOR : BLACK;
        previewGrid.appendChild(pixel);
    });
}

function addToHistory() {
    const currentState = captureGridState();

    if (historyIndex < drawingHistory.length - 1) {
        drawingHistory = drawingHistory.slice(0, historyIndex + 1);
    }

    drawingHistory.push(currentState);
    historyIndex++;

    if (drawingHistory.length > HISTORY_BUFFER_SIZE) {
        drawingHistory.shift();
        historyIndex--;
    }

    persistGridState(currentState);
    updateHistoryButtons();
}

function undo() {
    if (historyIndex > 0) {
        historyIndex--;
        restoreGridFromState(drawingHistory[historyIndex]);
        persistGridState(drawingHistory[historyIndex]);
    }
    updateHistoryButtons();
}

function redo() {
    if (historyIndex < drawingHistory.length - 1) {
        historyIndex++;
        restoreGridFromState(drawingHistory[historyIndex]);
        persistGridState(drawingHistory[historyIndex]);
    }
    updateHistoryButtons();
}

function updateHistoryButtons() {
    const undoBtn = document.getElementById('undoBtn');
    const redoBtn = document.getElementById('redoBtn');
    if (undoBtn) undoBtn.disabled = historyIndex <= 0;
    if (redoBtn) redoBtn.disabled = historyIndex >= drawingHistory.length - 1;
}

// Initialize grid history after the grid exists
document.addEventListener('DOMContentLoaded', function() {
    const savedState = loadSavedGridState();
    if (savedState) {
        restoreGridFromState(savedState);
    }

    const initialState = captureGridState();
    drawingHistory = [initialState];
    historyIndex = 0;

    const undoBtn = document.getElementById('undoBtn');
    const redoBtn = document.getElementById('redoBtn');
    const isMac = /Mac|iPhone|iPad|iPod/.test(navigator.platform);
    const mod = isMac ? '⌘' : 'Ctrl+';
    if (undoBtn) {
        undoBtn.title = `Undo (${mod}Z)`;
        undoBtn.addEventListener('click', undo);
    }
    if (redoBtn) {
        redoBtn.title = `Redo (${isMac ? '⇧⌘Z' : 'Ctrl+Y'})`;
        redoBtn.addEventListener('click', redo);
    }
    updateHistoryButtons();

    updateUploadRotatePrompt();
    requestAnimationFrame(() => {
        requestAnimationFrame(updateUploadRotatePrompt);
    });
});

window.addEventListener('resize', updateUploadRotatePrompt);
window.addEventListener('orientationchange', updateUploadRotatePrompt);
window.addEventListener('load', updateUploadRotatePrompt);

// Keyboard shortcuts for undo/redo
document.addEventListener('keydown', function(e) {
    const isMac = /Mac|iPhone|iPad|iPod/.test(navigator.platform);
    const modifier = isMac ? e.metaKey : e.ctrlKey;

    if (modifier && e.key === 'z' && !e.shiftKey) {
        e.preventDefault();
        undo();
    } else if (modifier && (e.key === 'z' && e.shiftKey || e.key === 'y')) {
        e.preventDefault();
        redo();
    }
});

// Get grid position from pixel index
function getGridPosition(index) {
    return {
        x: index % GRID_WIDTH,
        y: Math.floor(index / GRID_WIDTH)
    };
}

// Get pixel index from grid position
function getPixelIndex(x, y) {
    if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) {
        return -1;
    }
    return y * GRID_WIDTH + x;
}

// Bresenham's line algorithm to get all pixels between two points
function getPixelsBetween(x0, y0, x1, y1) {
    const pixels = [];
    const dx = Math.abs(x1 - x0);
    const dy = Math.abs(y1 - y0);
    const sx = x0 < x1 ? 1 : -1;
    const sy = y0 < y1 ? 1 : -1;
    let err = dx - dy;

    let x = x0;
    let y = y0;

    while (true) {
        const index = getPixelIndex(x, y);
        if (index !== -1) {
            pixels.push(index);
        }

        if (x === x1 && y === y1) break;

        const e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }

    return pixels;
}

// Tool / color state
let currentTool = 'pencil'; // 'pencil' | 'line' | 'fill'
let currentColor = 'blue';  // 'blue' | 'black' | 'dither'
let penSize = 1;            // 1..20

function ditherActiveAt(x, y) {
    return ((x + y) & 1) === 1;
}

function setPixelToCurrentColor(pixel) {
    if (currentColor === 'dither') {
        const { x, y } = getGridPosition(parseInt(pixel.dataset.index));
        if (ditherActiveAt(x, y)) {
            pixel.classList.add('active');
        } else {
            pixel.classList.remove('active');
        }
        return;
    }
    if (currentColor === 'blue') {
        pixel.classList.add('active');
    } else {
        pixel.classList.remove('active');
    }
}

// Returns pixel indices for a square brush of side `penSize` centered on (cx, cy).
function getBrushIndices(cx, cy) {
    const indices = [];
    const half = Math.floor((penSize - 1) / 2);
    for (let dy = 0; dy < penSize; dy++) {
        for (let dx = 0; dx < penSize; dx++) {
            const idx = getPixelIndex(cx - half + dx, cy - half + dy);
            if (idx !== -1) indices.push(idx);
        }
    }
    return indices;
}

// Brush-stamp every center along a line, returning the unique set of pixel indices touched.
function getStrokeIndices(centerIndices) {
    const set = new Set();
    centerIndices.forEach(idx => {
        const { x, y } = getGridPosition(idx);
        getBrushIndices(x, y).forEach(i => set.add(i));
    });
    return set;
}

function floodFill(startIndex) {
    const startPixel = document.querySelector(`[data-index="${startIndex}"]`);
    if (!startPixel) return false;

    const startActive = startPixel.classList.contains('active');
    if (currentColor !== 'dither') {
        const targetActive = currentColor === 'blue';
        if (startActive === targetActive) return false;
    }

    const visited = new Set();
    const stack = [startIndex];
    let changed = false;

    while (stack.length) {
        const idx = stack.pop();
        if (visited.has(idx)) continue;
        visited.add(idx);

        const pixel = document.querySelector(`[data-index="${idx}"]`);
        if (!pixel) continue;
        if (pixel.classList.contains('active') !== startActive) continue;

        const before = pixel.classList.contains('active');
        setPixelToCurrentColor(pixel);
        if (pixel.classList.contains('active') !== before) changed = true;

        const { x, y } = getGridPosition(idx);
        const neighbors = [[1, 0], [-1, 0], [0, 1], [0, -1]];
        for (const [dx, dy] of neighbors) {
            const nIdx = getPixelIndex(x + dx, y + dy);
            if (nIdx !== -1 && !visited.has(nIdx)) {
                stack.push(nIdx);
            }
        }
    }

    return changed;
}

// Initialize pixel grid
function initializePixelGrid() {
    pixelGrid.innerHTML = '';
    for (let i = 0; i < GRID_WIDTH * GRID_HEIGHT; i++) {
        const pixel = document.createElement('div');
        pixel.className = 'pixel';
        pixel.dataset.index = i;

        pixelGrid.appendChild(pixel);
    }

    // Add drag drawing functionality
    let isDrawing = false;
    let lastPixelPos = null;
    let didChange = false;

    // Line tool state
    let isDrawingLine = false;
    let lineStartPos = null;
    let lineStartGridState = null;
    let lineLastEndPos = null;

    function drawToClientPoint(clientX, clientY) {
        const pixelUnderPointer = document.elementFromPoint(clientX, clientY);
        if (!pixelUnderPointer || !pixelUnderPointer.classList.contains('pixel')) {
            return;
        }

        const index = parseInt(pixelUnderPointer.dataset.index);
        const currentPixelPos = getGridPosition(index);

        const centers = getPixelsBetween(
            lastPixelPos.x,
            lastPixelPos.y,
            currentPixelPos.x,
            currentPixelPos.y
        );

        getStrokeIndices(centers).forEach(pixelIndex => {
            const pixel = document.querySelector(`[data-index="${pixelIndex}"]`);
            if (pixel) {
                setPixelToCurrentColor(pixel);
                didChange = true;
            }
        });

        lastPixelPos = currentPixelPos;
    }

    function pixelPosFromClientPoint(clientX, clientY) {
        const el = document.elementFromPoint(clientX, clientY);
        if (el && el.classList.contains('pixel')) {
            return getGridPosition(parseInt(el.dataset.index));
        }
        const rect = pixelGrid.getBoundingClientRect();
        const x = Math.max(0, Math.min(GRID_WIDTH - 1,
            Math.floor((clientX - rect.left) / rect.width * GRID_WIDTH)));
        const y = Math.max(0, Math.min(GRID_HEIGHT - 1,
            Math.floor((clientY - rect.top) / rect.height * GRID_HEIGHT)));
        return { x, y };
    }

    function renderLinePreview(endPos) {
        // Restore canvas to pre-line state
        const pixels = document.querySelectorAll('.pixel');
        pixels.forEach((pixel, index) => {
            if (lineStartGridState[index]) {
                pixel.classList.add('active');
            } else {
                pixel.classList.remove('active');
            }
        });
        // Paint the preview line, brush-stamped at each center.
        const centers = getPixelsBetween(lineStartPos.x, lineStartPos.y, endPos.x, endPos.y);
        getStrokeIndices(centers).forEach(idx => {
            const pixel = document.querySelector(`[data-index="${idx}"]`);
            if (pixel) setPixelToCurrentColor(pixel);
        });
    }

    function stopDrawing() {
        if (isDrawingLine) {
            const endPos = lineLastEndPos || lineStartPos;
            renderLinePreview(endPos);
            // Commit if final state differs from starting state
            const pixels = document.querySelectorAll('.pixel');
            let changed = false;
            for (let i = 0; i < pixels.length; i++) {
                if (pixels[i].classList.contains('active') !== lineStartGridState[i]) {
                    changed = true;
                    break;
                }
            }
            if (changed) addToHistory();
            isDrawingLine = false;
            lineStartPos = null;
            lineStartGridState = null;
            lineLastEndPos = null;
            return;
        }

        if (!isDrawing) {
            return;
        }

        if (didChange) {
            addToHistory();
        }
        isDrawing = false;
        lastPixelPos = null;
        didChange = false;
    }

    pixelGrid.addEventListener('pointerdown', function(e) {
        if (!e.target.classList.contains('pixel')) {
            return;
        }

        e.preventDefault();
        const index = parseInt(e.target.dataset.index);

        if (currentTool === 'fill') {
            if (floodFill(index)) {
                addToHistory();
            }
            return;
        }

        if (currentTool === 'line') {
            isDrawingLine = true;
            lineStartPos = getGridPosition(index);
            lineLastEndPos = lineStartPos;
            lineStartGridState = captureGridState();
            renderLinePreview(lineStartPos);
            if (typeof pixelGrid.setPointerCapture === 'function') {
                pixelGrid.setPointerCapture(e.pointerId);
            }
            return;
        }

        // Pencil
        isDrawing = true;
        lastPixelPos = getGridPosition(index);
        didChange = false;
        getBrushIndices(lastPixelPos.x, lastPixelPos.y).forEach(idx => {
            const pixel = document.querySelector(`[data-index="${idx}"]`);
            if (pixel) setPixelToCurrentColor(pixel);
        });
        didChange = true;

        if (typeof pixelGrid.setPointerCapture === 'function') {
            pixelGrid.setPointerCapture(e.pointerId);
        }
    });

    pixelGrid.addEventListener('pointermove', function(e) {
        if (isDrawingLine) {
            e.preventDefault();
            const endPos = pixelPosFromClientPoint(e.clientX, e.clientY);
            lineLastEndPos = endPos;
            renderLinePreview(endPos);
            return;
        }

        if (!isDrawing) {
            return;
        }

        e.preventDefault();
        drawToClientPoint(e.clientX, e.clientY);
    });

    pixelGrid.addEventListener('pointerup', function(e) {
        e.preventDefault();
        stopDrawing();
    });

    pixelGrid.addEventListener('pointercancel', stopDrawing);
    pixelGrid.addEventListener('lostpointercapture', stopDrawing);

    ['touchstart', 'touchmove'].forEach(eventName => {
        pixelGrid.addEventListener(eventName, function(e) {
            e.preventDefault();
        }, { passive: false });
    });
}

// Tool / color selectors
document.querySelectorAll('.tool-btn').forEach(btn => {
    btn.addEventListener('click', function() {
        currentTool = this.dataset.tool;
        document.querySelectorAll('.tool-btn').forEach(b => b.classList.toggle('active', b === this));
    });
});

const penSizeSlider = document.getElementById('penSizeSlider');
const penSizeValue = document.getElementById('penSizeValue');
if (penSizeSlider) {
    penSizeSlider.addEventListener('input', function() {
        penSize = Math.max(1, Math.min(20, parseInt(this.value, 10) || 1));
        if (penSizeValue) penSizeValue.textContent = String(penSize);
    });
}

document.querySelectorAll('.color-btn').forEach(btn => {
    btn.addEventListener('click', function() {
        currentColor = this.dataset.color;
        document.querySelectorAll('.color-btn').forEach(b => b.classList.toggle('active', b === this));
    });
});

// Clear button — open confirmation modal
const clearConfirmModal = document.getElementById('clearConfirmModal');
clearPixelsBtn.addEventListener('click', function() {
    clearConfirmModal.classList.remove('hidden');
});

document.getElementById('cancelClearModal').addEventListener('click', function() {
    clearConfirmModal.classList.add('hidden');
});

document.getElementById('confirmClearModal').addEventListener('click', function() {
    const pixels = document.querySelectorAll('.pixel');
    pixels.forEach(pixel => {
        pixel.classList.remove('active');
    });
    addToHistory();
    clearConfirmModal.classList.add('hidden');
});

clearConfirmModal.addEventListener('click', function(e) {
    if (e.target === this) {
        clearConfirmModal.classList.add('hidden');
    }
});

// Initialize pixel grid on page load
initializePixelGrid();

// Handle upload button click
uploadBtn.addEventListener('click', function() {
    fileInput.click();
});

// Open name modal when sending to queue
sendQueueBtn.addEventListener('click', function() {
    const currentState = captureGridState();
    renderPreview(currentState, 'nameModalPreview');
    setQueueStatus('');
    document.getElementById('pictureName').value = '';
    document.getElementById('pictureNameError').classList.add('hidden');
    document.getElementById('nameModal').classList.remove('hidden');
    document.getElementById('pictureName').focus();
});

// Store the grid state before uploading
let gridStateBeforeUpload = [];

// Store processed image data
let currentProcessedImage = null;

// Crop state
let cropState = {
    offsetX: 0,
    offsetY: 0,
    scale: 1,
    isDragging: false,
    dragStartX: 0,
    dragStartY: 0,
    image: null
};

// Save current grid state
function saveGridState() {
    const pixels = document.querySelectorAll('.pixel');
    gridStateBeforeUpload = Array.from(pixels).map(pixel =>
        pixel.classList.contains('active')
    );
}

// Restore grid state
function restoreGridState() {
    const pixels = document.querySelectorAll('.pixel');
    pixels.forEach((pixel, index) => {
        if (gridStateBeforeUpload[index]) {
            pixel.classList.add('active');
        } else {
            pixel.classList.remove('active');
        }
    });

    persistGridState(gridStateBeforeUpload);
}

// Get cropped image as canvas
function getCroppedImage() {
    const canvas = document.getElementById('cropCanvas');
    const displayWidth = canvas.offsetWidth;
    const displayHeight = canvas.offsetHeight;

    const resultCanvas = document.createElement('canvas');
    resultCanvas.width = displayWidth;
    resultCanvas.height = displayHeight;
    const ctx = resultCanvas.getContext('2d');

    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, displayWidth, displayHeight);

    if (cropState.image) {
        ctx.save();
        ctx.translate(cropState.offsetX, cropState.offsetY);
        ctx.scale(cropState.scale, cropState.scale);
        ctx.drawImage(cropState.image, 0, 0);
        ctx.restore();
    }

    return resultCanvas;
}

// Update crop image display
function updateCropDisplay() {
    const canvas = document.getElementById('cropCanvas');
    if (!canvas || !cropState.image) return;

    const ctx = canvas.getContext('2d');
    const displayWidth = canvas.offsetWidth;
    const displayHeight = canvas.offsetHeight;

    canvas.width = displayWidth;
    canvas.height = displayHeight;

    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, displayWidth, displayHeight);

    ctx.save();
    ctx.translate(cropState.offsetX, cropState.offsetY);
    ctx.scale(cropState.scale, cropState.scale);
    ctx.drawImage(cropState.image, 0, 0);
    ctx.restore();
}

// Initialize crop modal
function initializeCropModal(imageUrl) {
    const cropModal = document.getElementById('cropModal');
    const canvas = document.getElementById('cropCanvas');

    cropModal.classList.remove('hidden');

    const img = new Image();
    img.onload = function() {
        createImageBitmap(img).then(bitmap => {
            cropState.image = bitmap;

            const displayWidth = canvas.offsetWidth;
            const displayHeight = canvas.offsetHeight;
            const frameAspect = displayWidth / displayHeight;
            const imageAspect = img.width / img.height;

            if (imageAspect > frameAspect) {
                cropState.scale = displayHeight / img.height;
            } else {
                cropState.scale = displayWidth / img.width;
            }

            const scaledWidth = img.width * cropState.scale;
            const scaledHeight = img.height * cropState.scale;
            cropState.offsetX = (displayWidth - scaledWidth) / 2;
            cropState.offsetY = (displayHeight - scaledHeight) / 2;

            updateCropDisplay();
            setTimeout(setupCropFrameListeners, 100);
        });
    };
    img.crossOrigin = 'anonymous';
    img.src = imageUrl;
}

// Set up crop frame event listeners
function setupCropFrameListeners() {
    const canvas = document.getElementById('cropCanvas');
    if (!canvas) return;

    canvas.addEventListener('mousedown', function(e) {
        cropState.isDragging = true;
        cropState.dragStartX = e.clientX - cropState.offsetX;
        cropState.dragStartY = e.clientY - cropState.offsetY;
    });

    document.addEventListener('mousemove', function(e) {
        if (!cropState.isDragging || !cropState.image) return;

        const canvas = document.getElementById('cropCanvas');
        if (!canvas) return;

        cropState.offsetX = e.clientX - cropState.dragStartX;
        cropState.offsetY = e.clientY - cropState.dragStartY;

        const scaledWidth = cropState.image.width * cropState.scale;
        const scaledHeight = cropState.image.height * cropState.scale;
        const displayWidth = canvas.offsetWidth;
        const displayHeight = canvas.offsetHeight;

        if (scaledWidth >= displayWidth) {
            cropState.offsetX = Math.min(0, Math.max(cropState.offsetX, displayWidth - scaledWidth));
        } else {
            cropState.offsetX = Math.max(displayWidth - scaledWidth, Math.min(0, cropState.offsetX));
        }

        if (scaledHeight >= displayHeight) {
            cropState.offsetY = Math.min(0, Math.max(cropState.offsetY, displayHeight - scaledHeight));
        } else {
            cropState.offsetY = Math.max(displayHeight - scaledHeight, Math.min(0, cropState.offsetY));
        }

        updateCropDisplay();
    });

    document.addEventListener('mouseup', function() {
        cropState.isDragging = false;
    });

    canvas.addEventListener('wheel', function(e) {
        e.preventDefault();
        if (!cropState.image) return;

        const rect = canvas.getBoundingClientRect();
        const mouseX = e.clientX - rect.left;
        const mouseY = e.clientY - rect.top;
        const displayWidth = canvas.offsetWidth;
        const displayHeight = canvas.offsetHeight;

        const zoomSpeed = 0.03;
        const oldScale = cropState.scale;

        const minScaleX = displayWidth / cropState.image.width;
        const minScaleY = displayHeight / cropState.image.height;
        const minScale = Math.min(minScaleX, minScaleY);

        cropState.scale = Math.max(minScale, Math.min(3, cropState.scale * (e.deltaY > 0 ? (1 - zoomSpeed) : (1 + zoomSpeed))));

        cropState.offsetX = mouseX - (mouseX - cropState.offsetX) * (cropState.scale / oldScale);
        cropState.offsetY = mouseY - (mouseY - cropState.offsetY) * (cropState.scale / oldScale);

        updateCropDisplay();
    }, { passive: false });
}

// Process image: downscale to 37x18, convert to black/white based on threshold
function processImage(canvas, blackPoint) {
    const processCanvas = document.createElement('canvas');
    processCanvas.width = GRID_WIDTH;
    processCanvas.height = GRID_HEIGHT;
    const ctx = processCanvas.getContext('2d');

    ctx.drawImage(canvas, 0, 0, GRID_WIDTH, GRID_HEIGHT);

    const imageData = ctx.getImageData(0, 0, GRID_WIDTH, GRID_HEIGHT);
    const data = imageData.data;

    const processedPixels = [];
    for (let i = 0; i < data.length; i += 4) {
        const r = data[i];
        const g = data[i + 1];
        const b = data[i + 2];

        const gray = 0.299 * r + 0.587 * g + 0.114 * b;
        processedPixels.push(gray < blackPoint);
    }

    return processedPixels;
}

// Update preview grid based on processed image and black point
function updatePreview(blackPoint) {
    if (!currentProcessedImage) return;

    const processedPixels = processImage(currentProcessedImage, blackPoint);
    const previewGrid = document.getElementById('previewGrid');
    const previewPixels = previewGrid.querySelectorAll('.preview-pixel');

    previewPixels.forEach((pixel, index) => {
        pixel.style.backgroundColor = processedPixels[index] ? '#000000' : '#02b2d9';
    });
}

// Lazy-load heic2any from CDN. iOS Safari can decode HEIC natively into an
// <img>, but desktop browsers (and most Android) cannot — so we always run
// HEIC files through heic2any to get a JPEG blob the rest of the pipeline
// (FileReader → <img> in initializeCropModal) can handle.
let heic2anyLoader = null;
function loadHeic2any() {
    if (window.heic2any) return Promise.resolve(window.heic2any);
    if (heic2anyLoader) return heic2anyLoader;
    heic2anyLoader = new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = 'https://cdn.jsdelivr.net/npm/heic2any@0.0.4/dist/heic2any.min.js';
        s.onload = () => resolve(window.heic2any);
        s.onerror = () => {
            heic2anyLoader = null;
            reject(new Error('Failed to load HEIC decoder'));
        };
        document.head.appendChild(s);
    });
    return heic2anyLoader;
}

function isHeicFile(file) {
    const t = (file.type || '').toLowerCase();
    if (t === 'image/heic' || t === 'image/heif') return true;
    return /\.(heic|heif)$/i.test(file.name || '');
}

// Handle file selection
fileInput.addEventListener('change', async function(event) {
    const file = event.target.files[0];
    if (!file) return;

    saveGridState();

    let blob = file;
    if (isHeicFile(file)) {
        setQueueStatus('Converting HEIC…');
        try {
            const heic2any = await loadHeic2any();
            const result = await heic2any({ blob: file, toType: 'image/jpeg', quality: 0.9 });
            blob = Array.isArray(result) ? result[0] : result;
            setQueueStatus('');
        } catch (err) {
            console.error('HEIC conversion failed', err);
            setQueueStatus('Could not read HEIC image.', true);
            fileInput.value = '';
            return;
        }
    }

    const reader = new FileReader();
    reader.onload = function(e) {
        initializeCropModal(e.target.result);
    };
    reader.readAsDataURL(blob);
});

// Handle confirm crop
document.getElementById('confirmCrop').addEventListener('click', function() {
    if (!cropState.image) return;

    const croppedCanvas = getCroppedImage();
    currentProcessedImage = croppedCanvas;

    document.getElementById('cropModal').classList.add('hidden');

    const uploadModal = document.getElementById('uploadModal');
    uploadModal.classList.remove('hidden');

    const previewGrid = document.getElementById('previewGrid');
    previewGrid.innerHTML = '';
    for (let i = 0; i < GRID_WIDTH * GRID_HEIGHT; i++) {
        const pixel = document.createElement('div');
        pixel.className = 'preview-pixel';
        previewGrid.appendChild(pixel);
    }

    const slider = document.getElementById('blackPointSlider');
    slider.value = '127';
    document.getElementById('blackPointValue').textContent = '127';
    updatePreview(127);
});

// Handle cancel crop
document.getElementById('cancelCrop').addEventListener('click', function() {
    restoreGridState();
    document.getElementById('cropModal').classList.add('hidden');
    fileInput.value = '';
    cropState.image = null;
    currentProcessedImage = null;
});

// Handle black point slider changes
document.getElementById('blackPointSlider').addEventListener('input', function(e) {
    const blackPoint = parseInt(e.target.value);
    document.getElementById('blackPointValue').textContent = blackPoint;
    updatePreview(blackPoint);
});

// Handle confirm upload
document.getElementById('confirmUpload').addEventListener('click', function() {
    if (!currentProcessedImage) return;

    const slider = document.getElementById('blackPointSlider');
    const blackPoint = parseInt(slider.value);
    const processedPixels = processImage(currentProcessedImage, blackPoint);

    const pixels = document.querySelectorAll('.pixel');
    pixels.forEach((pixel, index) => {
        if (!processedPixels[index]) {
            pixel.classList.add('active');
        } else {
            pixel.classList.remove('active');
        }
    });

    addToHistory();
    document.getElementById('uploadModal').classList.add('hidden');
    fileInput.value = '';
    currentProcessedImage = null;
});

// Handle cancel upload
document.getElementById('cancelUpload').addEventListener('click', function() {
    restoreGridState();
    document.getElementById('uploadModal').classList.add('hidden');
    fileInput.value = '';
    currentProcessedImage = null;
});

document.getElementById('pictureName').addEventListener('input', function() {
    if (this.value.trim()) {
        document.getElementById('pictureNameError').classList.add('hidden');
        this.classList.remove('input-error');
    }
});

// Name modal — cancel
document.getElementById('cancelNameModal').addEventListener('click', function() {
    document.getElementById('nameModal').classList.add('hidden');
});

// Name modal — upload
document.getElementById('confirmNameModal').addEventListener('click', async function() {
    const nameModal = document.getElementById('nameModal');
    const nameInput = document.getElementById('pictureName');
    const nameError = document.getElementById('pictureNameError');
    const name = nameInput.value.trim();

    if (!name) {
        nameError.classList.remove('hidden');
        nameInput.classList.add('input-error');
        nameInput.focus();
        return;
    }
    nameError.classList.add('hidden');
    nameInput.classList.remove('input-error');
    const state = captureGridState();
    const payload = {
        item: encodeGridStateToBase64(state),
        name: name
    };

    setQueueStatus('Sending to queue...');

    try {
        const response = await fetch('/enqueue.php', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(payload)
        });

        const result = await response.json();
        if (!response.ok) {
            throw new Error(getQueueErrorMessage(result.error, response.status));
        }

        if (!result.ok) {
            throw new Error(getQueueErrorMessage(result.error));
        }

        setQueueStatus('Picture added to the P.A.R. queue.');
    } catch (error) {
        console.error('Error sending picture to queue:', error);
        const message = error instanceof Error
            ? error.message
            : 'Could not add picture to the P.A.R. queue.';
        setQueueStatus(message, true);
    } finally {
        nameModal.classList.add('hidden');
    }
});
