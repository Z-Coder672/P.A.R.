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

    // Scoped to the menu: the brand is also a [data-tab="latest"] link, and an
    // unscoped querySelector would hand it the active state (and its underline)
    // instead of the Latest menu item.
    const activeLink = document.querySelector(`.navbar-menu [data-tab="${tabName}"]`);
    if (activeLink) {
        activeLink.classList.add('active');
    }

    updateUploadRotatePrompt();

    if (tabName === 'gallery') {
        loadGallerySubtab(currentGallerySubtab);
        startGalleryPolling();
    } else {
        stopGalleryPolling();
    }

    if (tabName === 'latest') {
        loadLatestRecording();
    }
}

const GALLERY_POLL_INTERVAL_MS = 5000;
let galleryPollTimer = null;
let currentGallerySubtab = 'gallery';
let gallerySearchQuery = '';

function itemMatchesSearch(item) {
    if (!gallerySearchQuery) return true;
    const name = (item && item.name ? String(item.name) : '').toLowerCase();
    return name.includes(gallerySearchQuery);
}

function loadGallerySubtab(subtab) {
    if (subtab === 'queue') {
        loadQueue();
    } else {
        loadGallery();
    }
}

function startGalleryPolling() {
    stopGalleryPolling();
    galleryPollTimer = setInterval(() => {
        if (currentGallerySubtab === 'queue') {
            loadQueue({ silent: true });
        } else {
            loadGallery({ silent: true });
        }
    }, GALLERY_POLL_INTERVAL_MS);
}

function stopGalleryPolling() {
    if (galleryPollTimer !== null) {
        clearInterval(galleryPollTimer);
        galleryPollTimer = null;
    }
}

function setGallerySubtab(subtab) {
    currentGallerySubtab = subtab;
    document.querySelectorAll('.gallery-subtab').forEach(btn => {
        const isActive = btn.dataset.subtab === subtab;
        btn.classList.toggle('active', isActive);
        btn.setAttribute('aria-selected', isActive ? 'true' : 'false');
    });
    document.getElementById('galleryContainer').classList.toggle('hidden', subtab !== 'gallery');
    document.getElementById('queueContainer').classList.toggle('hidden', subtab !== 'queue');
    loadGallerySubtab(subtab);
}

// Determine active tab from URL
function getActiveTabFromUrl() {
    const pathname = window.location.pathname.replace(/\/$/, '');
    if (pathname === '/upload') return 'upload';
    if (pathname === '/gallery') return 'gallery';
    if (pathname === '/about') return 'about';
    return 'latest';
}

// --- YouTube IFrame Player API ---------------------------------------------
// Recordings embed via the IFrame Player API (not a bare <iframe>) so we can
// read player.getDuration() in the browser and seek to ~10s before the end —
// opening on the finished board. This needs ZERO YouTube Data API calls: the
// video id comes from gallery.php and the duration from the player itself.
const NEAR_END_SECONDS = 10;
let _ytApiReadyPromise = null;
let latestVideoPlayer = null;
let latestVideoToken = 0;
let galleryModalPlayer = null;
let galleryModalToken = 0;
let galleryModalAutoplayObserver = null;

function loadYouTubeIframeApi() {
    if (window.YT && window.YT.Player) return Promise.resolve();
    if (_ytApiReadyPromise) return _ytApiReadyPromise;
    _ytApiReadyPromise = new Promise((resolve) => {
        // The API invokes this global once loaded; chain any prior handler so we
        // don't clobber another registration.
        const prev = window.onYouTubeIframeAPIReady;
        window.onYouTubeIframeAPIReady = function () {
            if (typeof prev === 'function') prev();
            resolve();
        };
        const tag = document.createElement('script');
        tag.src = 'https://www.youtube.com/iframe_api';
        document.head.appendChild(tag);
    });
    return _ytApiReadyPromise;
}

// Mount a player on `mountEl` (the API replaces it with an iframe) that seeks to
// ~10s before the end as soon as the duration is known. getDuration() only
// returns a real value once playback metadata loads (≈ first PLAYING), so we try
// on both onReady and the first PLAYING. Returns the YT.Player synchronously —
// loadYouTubeIframeApi() must have resolved first.
function createNearEndPlayer(mountEl, videoId, { autoplay = false, onError, onReady } = {}) {
    let seeked = false;
    const seekNearEnd = (p) => {
        if (seeked) return;
        const dur = p.getDuration();
        if (dur && dur > NEAR_END_SECONDS) {
            p.seekTo(dur - NEAR_END_SECONDS, true);
            seeked = true;
        }
    };
    return new YT.Player(mountEl, {
        videoId,
        playerVars: {
            // Muted is the only autoplay browsers reliably allow; recordings are
            // silent anyway, so muting costs nothing.
            autoplay: autoplay ? 1 : 0,
            mute: autoplay ? 1 : 0,
            playsinline: 1,
            rel: 0,
        },
        events: {
            onReady: (e) => {
                seekNearEnd(e.target);
                if (autoplay) e.target.playVideo();
                if (typeof onReady === 'function') onReady(e.target);
            },
            onStateChange: (e) => {
                if (e.data === YT.PlayerState.PLAYING) seekNearEnd(e.target);
            },
            onError: () => { if (typeof onError === 'function') onError(); },
        },
    });
}

// Prints-done counter. Fed from the same gallery.php payload the latest
// recording uses, so it costs no extra request. A print counts as done once
// gallery.php reports pending=false (which it derives from the pending->info
// rename OR the presence of a real photo).
let printsDoneShown = null;

function updatePrintsDone(items) {
    const stat = document.getElementById('printsDoneStat');
    const valueEl = document.getElementById('printsDoneValue');
    const labelEl = document.getElementById('printsDoneLabel');
    if (!stat || !valueEl || !labelEl) return;

    const total = (items || []).filter(it => it && !it.pending).length;
    labelEl.textContent = total === 1 ? 'print done' : 'prints done';
    stat.classList.remove('hidden');

    if (printsDoneShown === total) return;

    // Count up on the first fill; jump straight to the value afterwards so a
    // poll-driven refresh doesn't re-run the animation.
    const from = printsDoneShown === null ? 0 : total;
    printsDoneShown = total;
    if (from === total) {
        valueEl.textContent = String(total);
        return;
    }

    const durationMs = 700;
    const start = performance.now();
    const step = (now) => {
        const t = Math.min(1, (now - start) / durationMs);
        const eased = 1 - Math.pow(1 - t, 3);
        valueEl.textContent = String(Math.round(from + (total - from) * eased));
        if (t < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
}

// Latest recording: pull the gallery list (newest-first) and embed the most
// recent entry that has a playable YouTube recording. No separate latest-video
// store — the gallery is the single source of truth.
async function loadLatestRecording() {
    const container = document.querySelector('.latest-container');
    if (!container) return;

    container.innerHTML = '';
    const loadingBox = document.createElement('div');
    loadingBox.className = 'latest-loading-box';
    loadingBox.textContent = 'Loading latest recording...';
    container.appendChild(loadingBox);

    const renderEmpty = () => {
        container.innerHTML = '';
        const box = document.createElement('div');
        box.className = 'no-latest-box';
        const p = document.createElement('p');
        p.textContent = 'No recordings yet —';
        const link = document.createElement('a');
        link.href = '/upload';
        link.className = 'nav-link';
        link.dataset.tab = 'upload';
        link.textContent = 'head to Upload to draw the first one.';
        link.addEventListener('click', function(e) {
            e.preventDefault();
            window.history.pushState({tab: 'upload'}, '', '/upload');
            showTab('upload');
        });
        box.appendChild(p);
        box.appendChild(link);
        container.appendChild(box);
    };

    const token = ++latestVideoToken;
    if (latestVideoPlayer) {
        try { latestVideoPlayer.destroy(); } catch (e) {}
        latestVideoPlayer = null;
    }

    let data;
    try {
        const resp = await fetch('/gallery.php');
        if (!resp.ok) throw new Error('gallery.php ' + resp.status);
        data = await resp.json();
        updatePrintsDone(data.items);
    } catch (err) {
        console.error('Error loading latest recording:', err);
        if (token !== latestVideoToken) return;
        container.innerHTML = '';
        const errorBox = document.createElement('div');
        errorBox.className = 'no-latest-box';
        errorBox.textContent = 'Error loading latest recording';
        container.appendChild(errorBox);
        return;
    }

    // Items are newest-first; keep the ones with a well-formed video id.
    const candidates = (data.items || []).filter(
        it => it.video_id && /^[A-Za-z0-9_-]{11}$/.test(it.video_id)
    );
    if (candidates.length === 0) {
        if (token === latestVideoToken) renderEmpty();
        return;
    }

    await loadYouTubeIframeApi();
    if (token !== latestVideoToken) return;   // a newer load superseded us

    container.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'latest-iframe-wrapper';
    const caption = document.createElement('p');
    caption.className = 'latest-caption';
    container.appendChild(wrapper);
    container.appendChild(caption);

    // Embed the newest candidate; if its video is gone / not embeddable
    // (onError), destroy it and fall through to the next-newest recording.
    let idx = 0;
    const tryNext = () => {
        if (token !== latestVideoToken) return;
        if (latestVideoPlayer) {
            try { latestVideoPlayer.destroy(); } catch (e) {}
            latestVideoPlayer = null;
        }
        wrapper.innerHTML = '';
        if (idx >= candidates.length) {
            renderEmpty();
            return;
        }
        const it = candidates[idx++];
        caption.textContent = it.name || 'Latest P.A.R. recording';
        const mount = document.createElement('div');
        wrapper.appendChild(mount);
        latestVideoPlayer = createNearEndPlayer(mount, it.video_id, {
            autoplay: true,
            onError: () => { if (token === latestVideoToken) setTimeout(tryNext, 0); },
        });
    };
    tryNext();
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

        const allItems = data.items || [];
        const items = allItems.filter(itemMatchesSearch);

        if (items.length === 0) {
            container.innerHTML = '';
            const placeholder = document.createElement('div');
            placeholder.className = 'gallery-placeholder';
            if (gallerySearchQuery && allItems.length > 0) {
                placeholder.innerHTML = `
                    <div class="gallery-placeholder-icon">
                        <i class="fa-solid fa-magnifying-glass"></i>
                    </div>
                    <p class="gallery-placeholder-text">No matches</p>
                    <p class="gallery-placeholder-sub">No gallery items match your search.</p>
                `;
            } else {
                placeholder.innerHTML = `
                    <div class="gallery-placeholder-icon">
                        <i class="fa-solid fa-image"></i>
                    </div>
                    <p class="gallery-placeholder-text">No pictures yet</p>
                    <p class="gallery-placeholder-sub">Pictures appear here after being displayed on the P.A.R.</p>
                `;
            }
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

// Queue
async function loadQueue({ silent = false } = {}) {
    const container = document.getElementById('queueContainer');
    if (!container) return;

    if (!silent) {
        container.innerHTML = '<p class="gallery-loading">Loading...</p>';
    }

    try {
        const response = await fetch('/queue.php');
        const data = await response.json();

        const allItems = data.items || [];
        const items = allItems.filter(itemMatchesSearch);

        if (items.length === 0) {
            container.innerHTML = '';
            const placeholder = document.createElement('div');
            placeholder.className = 'gallery-placeholder';
            if (gallerySearchQuery && allItems.length > 0) {
                placeholder.innerHTML = `
                    <div class="gallery-placeholder-icon">
                        <i class="fa-solid fa-magnifying-glass"></i>
                    </div>
                    <p class="gallery-placeholder-text">No matches</p>
                    <p class="gallery-placeholder-sub">No queued items match your search.</p>
                `;
            } else {
                placeholder.innerHTML = `
                    <div class="gallery-placeholder-icon">
                        <i class="fa-solid fa-hourglass-half"></i>
                    </div>
                    <p class="gallery-placeholder-text">Queue is empty</p>
                    <p class="gallery-placeholder-sub">Submitted pictures waiting for P.A.R. appear here.</p>
                `;
            }
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
            card.addEventListener('click', () => openQueueModal(item));

            grid.appendChild(card);
        });

        container.innerHTML = '';
        container.appendChild(grid);
    } catch (error) {
        console.error('Error loading queue:', error);
        if (!silent) {
            container.innerHTML = '<p class="gallery-loading">Error loading queue</p>';
        }
    }
}

function ordinalSuffix(n) {
    const v = n % 100;
    if (v >= 11 && v <= 13) return 'th';
    switch (n % 10) {
        case 1: return 'st';
        case 2: return 'nd';
        case 3: return 'rd';
        default: return 'th';
    }
}

// The artist credit is optional at every hop (blank on legacy entries, on
// submissions that skipped the field, and on the reset call), so the line is
// hidden outright rather than rendered as an empty "by".
function setGalleryModalArtist(el, artist) {
    const value = typeof artist === 'string' ? artist.trim() : '';
    if (!value) {
        el.textContent = '';
        el.classList.add('hidden');
        return;
    }
    el.textContent = `by ${value}`;
    el.classList.remove('hidden');
}

function openQueueModal(item) {
    const modal = document.getElementById('galleryItemModal');
    const nameEl = document.getElementById('galleryModalName');
    const imgEl = document.getElementById('galleryModalImage');
    const snapshotEl = document.getElementById('galleryModalSnapshot');
    const noSnapshotEl = document.getElementById('galleryModalNoSnapshot');
    const pendingEl = document.getElementById('galleryModalPending');
    const queueEl = document.getElementById('galleryModalQueue');
    const artistEl = document.getElementById('galleryModalArtist');

    imgEl.classList.add('hidden');
    snapshotEl.classList.add('hidden');
    snapshotEl.removeAttribute('src');
    noSnapshotEl.classList.add('hidden');
    pendingEl.classList.add('hidden');
    queueEl.classList.add('hidden');

    nameEl.textContent = item.name || '(unnamed)';
    setGalleryModalArtist(artistEl, item.artist);

    if (item.bitmap) {
        const src = bitmapBase64ToCanvas(item.bitmap);
        imgEl.width = src.width;
        imgEl.height = src.height;
        imgEl.getContext('2d').drawImage(src, 0, 0);
        imgEl.classList.remove('hidden');
    }

    const pos = item.position;
    queueEl.textContent = `In queue — ${pos}${ordinalSuffix(pos)} position`;
    queueEl.classList.remove('hidden');

    modal.classList.remove('hidden');
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

// Deep-linking: a `#<id>` hash opens that gallery entry's modal. The completion
// email links to `/gallery#<id>` so a recipient lands straight on their piece;
// opening any entry also writes the hash so the URL is shareable.
function galleryIdFromHash() {
    const m = window.location.hash.match(/^#(\d+)$/);
    return m ? m[1] : null;
}

async function openGalleryItemById(id) {
    try {
        const response = await fetch('/gallery.php');
        const data = await response.json();
        const item = (data.items || []).find(it => String(it.id) === String(id));
        if (item) openGalleryModal(item);
    } catch (error) {
        console.error('Error opening gallery item by id:', error);
    }
}

function openGalleryModal(item) {
    const modal = document.getElementById('galleryItemModal');
    const nameEl = document.getElementById('galleryModalName');
    const imgEl = document.getElementById('galleryModalImage');
    const snapshotEl = document.getElementById('galleryModalSnapshot');
    const noSnapshotEl = document.getElementById('galleryModalNoSnapshot');
    const pendingEl = document.getElementById('galleryModalPending');
    const queueEl = document.getElementById('galleryModalQueue');
    const videoEl = document.getElementById('galleryModalVideo');
    const artistEl = document.getElementById('galleryModalArtist');

    nameEl.textContent = '';
    setGalleryModalArtist(artistEl, '');
    imgEl.classList.add('hidden');
    snapshotEl.classList.add('hidden');
    snapshotEl.removeAttribute('src');
    noSnapshotEl.classList.add('hidden');
    pendingEl.classList.add('hidden');
    queueEl.classList.add('hidden');
    resetGalleryModalVideo();

    nameEl.textContent = item.name || '(unnamed)';
    setGalleryModalArtist(artistEl, item.artist);

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

    // Reflect the open entry in the URL so it can be copied/shared and survives
    // a refresh. replaceState (not push) keeps the back button leaving the tab.
    if (item.id !== undefined && item.id !== null && /^\d+$/.test(String(item.id))) {
        window.history.replaceState({ tab: 'gallery' }, '', '/gallery#' + item.id);
    }

    if (item.video_id && /^[A-Za-z0-9_-]{11}$/.test(item.video_id)) {
        maybeEmbedRecording(item.video_id, videoEl);
    }
}

// Tear down the modal's player and hide its container. Bumps the token so any
// in-flight maybeEmbedRecording (still awaiting the API) bails on resume.
function resetGalleryModalVideo() {
    galleryModalToken++;
    if (galleryModalAutoplayObserver) {
        galleryModalAutoplayObserver.disconnect();
        galleryModalAutoplayObserver = null;
    }
    if (galleryModalPlayer) {
        try { galleryModalPlayer.destroy(); } catch (e) {}
        galleryModalPlayer = null;
    }
    const videoEl = document.getElementById('galleryModalVideo');
    if (videoEl) {
        videoEl.innerHTML = '';
        videoEl.classList.add('hidden');
    }
}

async function maybeEmbedRecording(videoId, container) {
    const token = galleryModalToken;
    await loadYouTubeIframeApi();
    if (token !== galleryModalToken) return;   // modal moved on while API loaded

    container.innerHTML = '';
    const mount = document.createElement('div');
    container.appendChild(mount);
    container.classList.remove('hidden');
    let ready = false;
    let wantsPlay = false;
    const play = (p) => {
        // Muted is the only autoplay browsers reliably allow; the recordings are
        // silent anyway. seekNearEnd() then fires off the first PLAYING.
        try { p.mute(); p.playVideo(); } catch (e) {}
    };
    galleryModalPlayer = createNearEndPlayer(mount, videoId, {
        autoplay: false,
        onReady: (p) => {
            if (token !== galleryModalToken) return;
            ready = true;
            if (wantsPlay) play(p);
        },
        onError: () => {
            // Deleted / not embeddable — hide the video area entirely.
            if (token !== galleryModalToken) return;
            resetGalleryModalVideo();
        },
    });

    // Start playback once the embed is fully in view — immediately if it already
    // is when the modal opens. Fires once, then the observer is dropped so a
    // user pause isn't undone by scrolling away and back.
    startWhenFullyVisible(container, () => {
        if (token !== galleryModalToken || !galleryModalPlayer) return;
        if (ready) play(galleryModalPlayer);
        else wantsPlay = true;
    });
}

// Invoke `onVisible` once `el` is fully within the viewport. An embed taller
// than the viewport can never reach ratio 1, so anything that fills the visible
// area counts as fully in view. No IntersectionObserver -> play right away.
function startWhenFullyVisible(el, onVisible) {
    if (galleryModalAutoplayObserver) {
        galleryModalAutoplayObserver.disconnect();
        galleryModalAutoplayObserver = null;
    }
    if (!('IntersectionObserver' in window)) {
        onVisible();
        return;
    }
    const observer = new IntersectionObserver((entries) => {
        for (const entry of entries) {
            const h = entry.boundingClientRect.height;
            const viewportH = window.innerHeight || document.documentElement.clientHeight;
            const full = entry.intersectionRatio >= 0.99 ||
                (h > viewportH && entry.intersectionRect.height >= viewportH - 1);
            if (entry.isIntersecting && full) {
                observer.disconnect();
                if (galleryModalAutoplayObserver === observer) galleryModalAutoplayObserver = null;
                onVisible();
                return;
            }
        }
    }, { threshold: [0, 0.5, 0.9, 0.99, 1] });
    galleryModalAutoplayObserver = observer;
    observer.observe(el);
}

function closeGalleryModal() {
    resetGalleryModalVideo();
    document.getElementById('galleryItemModal').classList.add('hidden');
    // Drop the deep-link hash so a refresh doesn't re-open the modal.
    if (galleryIdFromHash() !== null) {
        window.history.replaceState({ tab: 'gallery' }, '', '/gallery');
    }
}

// Load livestream when page is ready
function updateNavbarHeightVar() {
    const navbar = document.querySelector('.navbar');
    if (!navbar) return;
    const h = navbar.getBoundingClientRect().height;
    document.documentElement.style.setProperty('--navbar-height', `${h}px`);
}

window.addEventListener('resize', updateNavbarHeightVar);
window.addEventListener('orientationchange', updateNavbarHeightVar);

// Scroll-reveal: reveal .reveal elements once as they scroll into view.
// Falls back to showing everything if IntersectionObserver is unavailable or
// the user prefers reduced motion.
function initScrollReveal() {
    const els = document.querySelectorAll('.reveal');
    const reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (reduce || !('IntersectionObserver' in window)) {
        els.forEach(el => el.classList.add('in-view'));
        return;
    }
    const observer = new IntersectionObserver((entries, obs) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                entry.target.classList.add('in-view');
                obs.unobserve(entry.target);
            }
        });
    }, { rootMargin: '0px 0px -10% 0px', threshold: 0.12 });
    els.forEach(el => observer.observe(el));
}

function observeGallerySubtabsStuck() {
    const sentinel = document.querySelector('.gallery-subtabs-sentinel');
    const subtabs = document.querySelector('.gallery-subtabs');
    if (!sentinel || !subtabs || !('IntersectionObserver' in window)) return;

    const navH = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--navbar-height')) || 64;
    const observer = new IntersectionObserver(
        ([entry]) => {
            subtabs.classList.toggle('stuck', !entry.isIntersecting);
        },
        { rootMargin: `-${navH}px 0px 0px 0px`, threshold: 0 }
    );
    observer.observe(sentinel);
}

document.addEventListener('DOMContentLoaded', function() {
    updateNavbarHeightVar();
    observeGallerySubtabsStuck();
    initScrollReveal();
    loadLatestRecording();

    document.querySelectorAll('.gallery-subtab').forEach(btn => {
        btn.addEventListener('click', function() {
            setGallerySubtab(this.dataset.subtab);
        });
    });

    const gallerySearchInput = document.getElementById('gallerySearch');
    if (gallerySearchInput) {
        gallerySearchInput.addEventListener('input', function() {
            gallerySearchQuery = this.value.trim().toLowerCase();
            loadGallerySubtab(currentGallerySubtab);
        });
    }

    const howItWorksBtn = document.getElementById('howItWorksBtn');
    if (howItWorksBtn) {
        howItWorksBtn.addEventListener('click', function() {
            const target = document.getElementById('how-it-works');
            if (target) target.scrollIntoView({ behavior: 'smooth', block: 'start' });
        });
    }

    document.getElementById('closeGalleryModal').addEventListener('click', closeGalleryModal);
    document.getElementById('galleryItemModal').addEventListener('click', function(e) {
        if (e.target === this) closeGalleryModal();
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

// Open the matching gallery entry when the `#<id>` deep-link hash changes.
window.addEventListener('hashchange', function() {
    const id = galleryIdFromHash();
    if (id === null) return;
    if (getActiveTabFromUrl() !== 'gallery') {
        window.history.replaceState({ tab: 'gallery' }, '', '/gallery#' + id);
        showTab('gallery');
    }
    openGalleryItemById(id);
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

// Set initial active tab based on current URL. A `#<id>` hash (e.g. the deep
// link in a completion email) forces the gallery tab and opens that entry —
// even from a bare `/#<id>`, which is normalized to `/gallery#<id>`.
const _initialHashId = galleryIdFromHash();
if (_initialHashId !== null && getActiveTabFromUrl() !== 'gallery') {
    window.history.replaceState({ tab: 'gallery' }, '', '/gallery#' + _initialHashId);
}
showTab(getActiveTabFromUrl());
if (_initialHashId !== null) {
    openGalleryItemById(_initialHashId);
}

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
        return 'That picture has already been submitted.';
    }

    if (statusCode === 409 && errorCode === 'queue_full') {
        return 'Too many pictures are waiting for review. Try again later.';
    }

    if (statusCode === 400 && errorCode === 'Invalid bitmap payload') {
        return 'This picture could not be prepared for submission.';
    }

    if (statusCode === 429 || errorCode === 'rate_limited') {
        return 'You\'re sending pictures too quickly. Please wait a moment and try again.';
    }

    return 'Could not submit picture for review.';
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
    document.getElementById('pictureArtist').value = '';
    document.getElementById('nameModal').classList.remove('hidden');
    document.getElementById('pictureName').focus();
});

// Store the grid state before uploading
let gridStateBeforeUpload = [];

// Store processed image data
let currentProcessedImage = null;

// ---------------------------------------------------------------------------
// Photo crop: pan / zoom / rotate viewport
//
// Model: the view is center-based. `centerX/centerY` is where the image's own
// center sits in canvas CSS pixels, `scale` is CSS-pixels-per-image-pixel, and
// `rotation` is a quarter turn (0/90/180/270). Drawing is therefore
// translate(center) -> rotate -> scale -> drawImage(-w/2, -h/2), which keeps
// rotation independent of pan and zoom. Every input (mouse, touch, pen, wheel,
// keyboard, buttons) funnels through setCropScale/panCrop/clampCropOffsets, and
// painting is rAF-coalesced, so no handler ever draws more than once a frame.
// ---------------------------------------------------------------------------

const CROP_OUTPUT_SCALE = 20;      // crop is rasterized at 20x the 37x18 grid
const CROP_MAX_ZOOM_FACTOR = 16;   // max zoom, relative to the fit-whole-image scale
const CROP_MIN_ZOOM_FACTOR = 0.2;  // min zoom, relative to fit — lets you shrink into black margins
const CROP_KEEP_VISIBLE = 0.25;    // fraction of the overlap that must stay on screen while panning
const CROP_WHEEL_SENSITIVITY = 0.0025;

let cropState = {
    image: null,
    scale: 1,
    minScale: 1,
    maxScale: 1,
    rotation: 0,
    centerX: 0,
    centerY: 0,
    viewW: 0,
    viewH: 0
};

let cropCtx = null;
let cropDrawQueued = false;
let cropListenersBound = false;
let cropResizeObserver = null;
const cropPointers = new Map();   // pointerId -> {x, y}
let cropPinch = null;             // {dist, centroid} carried between move events

function cropCanvasEl() {
    return document.getElementById('cropCanvas');
}

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

// Image dimensions as they appear on screen at scale 1 — width and height swap
// on the quarter turns, which is what makes a portrait photo fit a landscape
// frame after a rotate.
function cropBaseSize() {
    const quarterTurned = cropState.rotation === 90 || cropState.rotation === 270;
    return {
        w: quarterTurned ? cropState.image.height : cropState.image.width,
        h: quarterTurned ? cropState.image.width : cropState.image.height
    };
}

// Fit = whole image inside the frame; cover = image fills the frame. Zooming
// out past `fit` is allowed (down to CROP_MIN_ZOOM_FACTOR) so a photo can be
// shrunk into black margins — the black is part of the crop and prints as off
// pixels, which is a legitimate composition, not a mistake.
function cropScaleBounds() {
    const base = cropBaseSize();
    const fit = Math.min(cropState.viewW / base.w, cropState.viewH / base.h);
    const cover = Math.max(cropState.viewW / base.w, cropState.viewH / base.h);
    return {
        fit,
        cover,
        min: fit * CROP_MIN_ZOOM_FACTOR,
        max: Math.max(fit * CROP_MAX_ZOOM_FACTOR, cover * 4)
    };
}

// The image may be dragged out past the frame edges — you often want a subject
// pushed to one side with black filling the rest. The only rule is that a
// sliver stays on screen, so it can never be flung out of reach.
function clampCropOffsets() {
    if (!cropState.image) return;

    const base = cropBaseSize();
    const dispW = base.w * cropState.scale;
    const dispH = base.h * cropState.scale;

    const keepX = Math.max(16, Math.min(dispW, cropState.viewW) * CROP_KEEP_VISIBLE);
    const keepY = Math.max(16, Math.min(dispH, cropState.viewH) * CROP_KEEP_VISIBLE);

    cropState.centerX = Math.min(
        Math.max(cropState.centerX, keepX - dispW / 2),
        cropState.viewW - keepX + dispW / 2
    );
    cropState.centerY = Math.min(
        Math.max(cropState.centerY, keepY - dispH / 2),
        cropState.viewH - keepY + dispH / 2
    );
}

// Zoom about a fixed point in canvas space, so the pixel under the cursor /
// pinch centroid stays put.
function setCropScale(nextScale, anchorX, anchorY) {
    if (!cropState.image) return;

    const clamped = Math.min(Math.max(nextScale, cropState.minScale), cropState.maxScale);
    if (clamped === cropState.scale) return;

    const ratio = clamped / cropState.scale;
    cropState.centerX = anchorX + (cropState.centerX - anchorX) * ratio;
    cropState.centerY = anchorY + (cropState.centerY - anchorY) * ratio;
    cropState.scale = clamped;

    clampCropOffsets();
    requestCropDraw();
}

function panCrop(dx, dy) {
    cropState.centerX += dx;
    cropState.centerY += dy;
    clampCropOffsets();
    requestCropDraw();
}

// Quarter-turn the view about the frame center, so whatever is framed stays
// framed. `dir` is +1 for clockwise, -1 for counter-clockwise.
function rotateCrop(dir) {
    if (!cropState.image) return;

    cropState.rotation = (cropState.rotation + (dir > 0 ? 90 : 270)) % 360;

    // Carry the image center around the same turn about the frame center.
    const fx = cropState.viewW / 2;
    const fy = cropState.viewH / 2;
    const vx = cropState.centerX - fx;
    const vy = cropState.centerY - fy;
    cropState.centerX = fx + (dir > 0 ? -vy : vy);
    cropState.centerY = fy + (dir > 0 ? vx : -vx);

    // Swapping width and height moves the zoom limits, so re-derive them and
    // pull the current zoom back into range.
    const b = cropScaleBounds();
    cropState.minScale = b.min;
    cropState.maxScale = b.max;
    cropState.scale = Math.min(Math.max(cropState.scale, b.min), b.max);

    clampCropOffsets();
    requestCropDraw();
}

// Fit the whole image inside the frame, centered. Keeps the current rotation.
function resetCropView() {
    if (!cropState.image || !cropState.viewW || !cropState.viewH) return;

    const b = cropScaleBounds();

    cropState.minScale = b.min;
    cropState.maxScale = b.max;
    cropState.scale = b.fit;
    cropState.centerX = cropState.viewW / 2;
    cropState.centerY = cropState.viewH / 2;

    requestCropDraw();
}

// Match the backing store to the CSS box (and DPR) without reallocating it on
// every frame. Preserves whatever the user is looking at across a resize or a
// device rotation by scaling the view about the frame center.
function syncCropCanvasSize() {
    const canvas = cropCanvasEl();
    if (!canvas) return;

    const rect = canvas.getBoundingClientRect();
    const cssW = Math.max(1, Math.round(rect.width));
    const cssH = Math.max(1, Math.round(rect.height));
    const dpr = window.devicePixelRatio || 1;
    const bw = Math.round(cssW * dpr);
    const bh = Math.round(cssH * dpr);

    const prevW = cropState.viewW;
    const prevH = cropState.viewH;

    if (canvas.width !== bw || canvas.height !== bh) {
        canvas.width = bw;
        canvas.height = bh;
        cropCtx = canvas.getContext('2d');
    } else if (!cropCtx) {
        cropCtx = canvas.getContext('2d');
    }

    cropState.viewW = cssW;
    cropState.viewH = cssH;
    cropCtx.setTransform(bw / cssW, 0, 0, bh / cssH, 0, 0);

    if (!cropState.image) return;

    if (prevW && prevH) {
        const k = cssW / prevW;
        cropState.centerX = cssW / 2 + (cropState.centerX - prevW / 2) * k;
        cropState.centerY = cssH / 2 + (cropState.centerY - prevH / 2) * (cssH / prevH);

        const b = cropScaleBounds();
        cropState.minScale = b.min;
        cropState.maxScale = b.max;
        cropState.scale = Math.min(Math.max(cropState.scale * k, b.min), b.max);
        clampCropOffsets();
    } else {
        resetCropView();
    }

    requestCropDraw();
}

function requestCropDraw() {
    if (cropDrawQueued) return;
    cropDrawQueued = true;
    requestAnimationFrame(() => {
        cropDrawQueued = false;
        drawCrop();
    });
}

// Shared by the on-screen preview and the exported crop, so what you see is
// exactly what gets pixelated.
function paintCrop(ctx) {
    ctx.save();
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.translate(cropState.centerX, cropState.centerY);
    ctx.rotate(cropState.rotation * Math.PI / 180);
    ctx.scale(cropState.scale, cropState.scale);
    ctx.drawImage(cropState.image, -cropState.image.width / 2, -cropState.image.height / 2);
    ctx.restore();
}

function drawCrop() {
    const canvas = cropCanvasEl();
    if (!canvas || !cropCtx) return;

    cropCtx.fillStyle = '#000000';
    cropCtx.fillRect(0, 0, cropState.viewW, cropState.viewH);

    if (!cropState.image) return;
    paintCrop(cropCtx);
}

// Rasterize the visible frame at 20x the LED grid, so processImage() has real
// detail to downsample from instead of a screen-resolution thumbnail.
function getCroppedImage() {
    const outW = GRID_WIDTH * CROP_OUTPUT_SCALE;
    const outH = GRID_HEIGHT * CROP_OUTPUT_SCALE;

    const resultCanvas = document.createElement('canvas');
    resultCanvas.width = outW;
    resultCanvas.height = outH;
    const ctx = resultCanvas.getContext('2d');

    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, outW, outH);

    if (cropState.image && cropState.viewW && cropState.viewH) {
        ctx.save();
        ctx.scale(outW / cropState.viewW, outH / cropState.viewH);
        paintCrop(ctx);
        ctx.restore();
    }

    return resultCanvas;
}

function cropPointerPos(e) {
    const rect = cropCanvasEl().getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
}

function cropPointerCentroid() {
    let sx = 0, sy = 0;
    cropPointers.forEach(p => { sx += p.x; sy += p.y; });
    return { x: sx / cropPointers.size, y: sy / cropPointers.size };
}

function cropPointerSpread() {
    const pts = Array.from(cropPointers.values());
    if (pts.length < 2) return 0;
    return Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y);
}

function onCropPointerDown(e) {
    const canvas = cropCanvasEl();
    if (!cropState.image) return;

    canvas.setPointerCapture(e.pointerId);
    cropPointers.set(e.pointerId, cropPointerPos(e));
    cropPinch = cropPointers.size >= 2
        ? { dist: cropPointerSpread(), centroid: cropPointerCentroid() }
        : { dist: 0, centroid: cropPointerCentroid() };
    canvas.classList.add('is-grabbing');
    e.preventDefault();
}

function onCropPointerMove(e) {
    if (!cropPointers.has(e.pointerId) || !cropState.image) return;
    e.preventDefault();

    cropPointers.set(e.pointerId, cropPointerPos(e));

    const centroid = cropPointerCentroid();
    const spread = cropPointerSpread();

    if (cropPointers.size >= 2 && cropPinch && cropPinch.dist > 0 && spread > 0) {
        // Pinch: zoom about the centroid, and let the centroid drag too.
        setCropScale(cropState.scale * (spread / cropPinch.dist), centroid.x, centroid.y);
    }

    if (cropPinch) {
        panCrop(centroid.x - cropPinch.centroid.x, centroid.y - cropPinch.centroid.y);
    }

    cropPinch = { dist: spread, centroid };
}

function onCropPointerUp(e) {
    const canvas = cropCanvasEl();
    if (!cropPointers.has(e.pointerId)) return;

    cropPointers.delete(e.pointerId);
    if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);

    // Re-baseline so lifting one finger of a pinch doesn't jump the image.
    cropPinch = cropPointers.size
        ? { dist: cropPointerSpread(), centroid: cropPointerCentroid() }
        : null;

    if (!cropPointers.size) canvas.classList.remove('is-grabbing');
}

function onCropWheel(e) {
    if (!cropState.image) return;
    e.preventDefault();

    // Normalize across deltaMode so a trackpad glides and a wheel notch steps.
    let delta = e.deltaY;
    if (e.deltaMode === 1) delta *= 16;
    else if (e.deltaMode === 2) delta *= cropState.viewH;

    const pos = cropPointerPos(e);
    setCropScale(cropState.scale * Math.exp(-delta * CROP_WHEEL_SENSITIVITY), pos.x, pos.y);
}

function onCropDoubleClick(e) {
    if (!cropState.image) return;
    e.preventDefault();

    const pos = cropPointerPos(e);
    const b = cropScaleBounds();

    if (cropState.scale > b.cover * 1.01) {
        resetCropView();
    } else {
        setCropScale(b.cover, pos.x, pos.y);
    }
}

function onCropKeyDown(e) {
    if (!cropState.image) return;

    const step = e.shiftKey ? 40 : 10;
    const cx = cropState.viewW / 2;
    const cy = cropState.viewH / 2;

    switch (e.key) {
        case 'ArrowLeft':  panCrop(step, 0); break;
        case 'ArrowRight': panCrop(-step, 0); break;
        case 'ArrowUp':    panCrop(0, step); break;
        case 'ArrowDown':  panCrop(0, -step); break;
        case '+':
        case '=':          setCropScale(cropState.scale * 1.2, cx, cy); break;
        case '-':
        case '_':          setCropScale(cropState.scale / 1.2, cx, cy); break;
        case '0':          resetCropView(); break;
        case 'r':          rotateCrop(1); break;
        case 'R':          rotateCrop(-1); break;
        default: return;
    }
    e.preventDefault();
}

// Bound exactly once for the lifetime of the page. The old code re-bound on
// every modal open, stacking a fresh document-level mousemove handler per photo.
function bindCropListeners() {
    if (cropListenersBound) return;
    const canvas = cropCanvasEl();
    if (!canvas) return;

    canvas.addEventListener('pointerdown', onCropPointerDown);
    canvas.addEventListener('pointermove', onCropPointerMove);
    canvas.addEventListener('pointerup', onCropPointerUp);
    canvas.addEventListener('pointercancel', onCropPointerUp);
    canvas.addEventListener('lostpointercapture', onCropPointerUp);
    canvas.addEventListener('wheel', onCropWheel, { passive: false });
    canvas.addEventListener('dblclick', onCropDoubleClick);
    canvas.addEventListener('keydown', onCropKeyDown);
    canvas.addEventListener('contextmenu', e => e.preventDefault());
    canvas.addEventListener('dragstart', e => e.preventDefault());

    const on = (id, fn) => {
        const el = document.getElementById(id);
        if (el) el.addEventListener('click', fn);
    };
    on('cropZoomIn', () => setCropScale(cropState.scale * 1.25, cropState.viewW / 2, cropState.viewH / 2));
    on('cropZoomOut', () => setCropScale(cropState.scale / 1.25, cropState.viewW / 2, cropState.viewH / 2));
    on('cropReset', resetCropView);
    on('cropRotateLeft', () => rotateCrop(-1));
    on('cropRotateRight', () => rotateCrop(1));

    if (window.ResizeObserver) {
        cropResizeObserver = new ResizeObserver(() => {
            if (!document.getElementById('cropModal').classList.contains('hidden')) {
                syncCropCanvasSize();
            }
        });
        cropResizeObserver.observe(canvas);
    } else {
        window.addEventListener('resize', syncCropCanvasSize);
    }

    cropListenersBound = true;
}

function closeCropModal() {
    document.getElementById('cropModal').classList.add('hidden');
    document.body.classList.remove('modal-open');
    cropPointers.clear();
    cropPinch = null;

    if (cropState.image && typeof cropState.image.close === 'function') {
        cropState.image.close();
    }
    cropState.image = null;
}

// Initialize crop modal
function initializeCropModal(imageSrc) {
    const cropModal = document.getElementById('cropModal');
    const canvas = cropCanvasEl();
    if (!canvas) return;

    const img = new Image();
    img.decoding = 'async';

    img.onload = async function() {
        let source = img;
        try {
            source = await createImageBitmap(img);
        } catch (err) {
            // Safari can refuse createImageBitmap on some sources; the <img>
            // draws identically, it just costs a bit more per frame.
            console.warn('createImageBitmap failed, drawing from <img>', err);
        }

        if (cropState.image && typeof cropState.image.close === 'function') {
            cropState.image.close();
        }

        cropState.image = source;
        cropState.rotation = 0;
        cropState.viewW = 0;
        cropState.viewH = 0;

        cropModal.classList.remove('hidden');
        document.body.classList.add('modal-open');

        bindCropListeners();
        // The modal must be visible before the canvas has a measurable box.
        syncCropCanvasSize();
        resetCropView();
    };

    img.onerror = function() {
        setQueueStatus('Could not read that image.', true);
        fileInput.value = '';
    };

    img.src = imageSrc;
}

// Process image: downscale to 37x18, convert to black/white based on threshold
function processImage(canvas, blackPoint) {
    const processCanvas = document.createElement('canvas');
    processCanvas.width = GRID_WIDTH;
    processCanvas.height = GRID_HEIGHT;
    const ctx = processCanvas.getContext('2d', { willReadFrequently: true });

    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
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

    closeCropModal();

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
    closeCropModal();
    fileInput.value = '';
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
    document.getElementById('pictureArtist').value = '';
    const emailInput = document.getElementById('pictureEmail');
    emailInput.value = '';
    emailInput.classList.remove('input-error');
    document.getElementById('pictureEmailError').classList.add('hidden');
});

// Name modal — upload
document.getElementById('confirmNameModal').addEventListener('click', async function() {
    const nameModal = document.getElementById('nameModal');
    const nameInput = document.getElementById('pictureName');
    const nameError = document.getElementById('pictureNameError');
    const artistInput = document.getElementById('pictureArtist');
    const emailInput = document.getElementById('pictureEmail');
    const emailError = document.getElementById('pictureEmailError');
    const name = nameInput.value.trim();

    if (!name) {
        nameError.classList.remove('hidden');
        nameInput.classList.add('input-error');
        nameInput.focus();
        return;
    }
    nameError.classList.add('hidden');
    nameInput.classList.remove('input-error');

    // Email is optional, but if provided it must look valid before we send.
    const email = emailInput.value.trim();
    if (email && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) {
        emailError.classList.remove('hidden');
        emailInput.classList.add('input-error');
        emailInput.focus();
        return;
    }
    emailError.classList.add('hidden');
    emailInput.classList.remove('input-error');

    const state = captureGridState();
    // Optional, like email: omitted from the payload entirely when blank, so
    // the server stores '' and the gallery skips the "by ..." line.
    const artist = artistInput.value.trim();

    const payload = {
        item: encodeGridStateToBase64(state),
        name: name
    };
    if (artist) {
        payload.artist = artist;
    }
    if (email) {
        payload.email = email;
    }

    setQueueStatus('Submitting for review...');

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

        setQueueStatus(email
            ? 'Picture submitted for review. Once approved it joins the P.A.R. queue, and we\'ll email you when it prints.'
            : 'Picture submitted for review. Once approved it joins the P.A.R. queue to be printed.');
        emailInput.value = '';
        artistInput.value = '';
    } catch (error) {
        console.error('Error sending picture to queue:', error);
        const message = error instanceof Error
            ? error.message
            : 'Could not submit picture for review.';
        setQueueStatus(message, true);
    } finally {
        nameModal.classList.add('hidden');
    }
});

document.getElementById('pictureEmail').addEventListener('input', function() {
    document.getElementById('pictureEmailError').classList.add('hidden');
    this.classList.remove('input-error');
});
