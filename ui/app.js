const GAMES_PER_PAGE = 24;

const shelfView = document.querySelector("#shelf-view");
const settingsView = document.querySelector("#settings-view");
const shelf = document.querySelector("#shelf");
const shelfPager = document.querySelector("#shelf-pager");
const shelfPagerInfo = document.querySelector("#shelf-pager-info");
const shelfPrev = document.querySelector("#shelf-prev");
const shelfNext = document.querySelector("#shelf-next");
const detailOverlay = document.querySelector("#detail-overlay");
const detailBackdrop = document.querySelector("#detail-backdrop");
const detailClose = document.querySelector("#detail-close");
const detail = document.querySelector("#detail");
const detailBody = document.querySelector("#detail-body");
const loginText = document.querySelector("#login-text");
const qr = document.querySelector("#qr");
const userCode = document.querySelector("#user-code");
const form = document.querySelector("#settings-form");
const settingsNote = document.querySelector("#settings-note");
const catalogStatus = document.querySelector("#catalog-status");
const catalogNote = document.querySelector("#catalog-note");
const catalogReload = document.querySelector("#catalog-reload");
const lede = document.querySelector("#lede");
const loginBox = document.querySelector("#login-box");

const signedOutHint = "登录百度网盘后才能看到游戏目录。";
const missingKeyHint = "请先打开设置，填写 AppKey 和 SecretKey 并保存。";
const covers = ["#d7c4a3", "#c9d3c4", "#d4c3b8", "#c5d0d6", "#e4d2b8", "#cfc6b4"];
const stateText = {
  installed: "已安装",
  continue: "继续",
  install: "安装",
  installing: "正在安装",
  unsupported: "还不能安装",
  empty: "没有压缩包",
  "need-settings": "先设置存放位置",
};

let games = [];
let currentPage = 1;
let pollTimer = 0;
let jobTimer = 0;
let detailEscapeHandler = null;

function totalPages() {
  if (games.length === 0) return 0;
  return Math.ceil(games.length / GAMES_PER_PAGE);
}

function clampPage(page) {
  const pages = totalPages();
  if (pages === 0) return 1;
  return Math.min(Math.max(1, page), pages);
}

function pageGames() {
  if (games.length === 0) return [];
  const page = clampPage(currentPage);
  const start = (page - 1) * GAMES_PER_PAGE;
  return games.slice(start, start + GAMES_PER_PAGE);
}

function renderPagination() {
  const pages = totalPages();
  const total = games.length;
  shelfPager.hidden = total === 0;
  if (total === 0) return;
  const page = clampPage(currentPage);
  currentPage = page;
  shelfPagerInfo.textContent = "第 " + page + " / " + pages + " 页，共 " + total + " 个";
  shelfPrev.disabled = page <= 1;
  shelfNext.disabled = page >= pages;
}

function goToPage(page) {
  if (games.length === 0) return;
  closeDetail();
  currentPage = clampPage(page);
  renderShelf();
}

function loginFlowActive() {
  return !qr.hidden || pollTimer !== 0;
}

function loginErrorText(message) {
  if (message === "缺少 PANPLAY_BAIDU_APP_KEY" || message === "缺少 PANPLAY_BAIDU_SECRET_KEY") return missingKeyHint;
  return message;
}

function stopLoginPoll() {
  window.clearInterval(pollTimer);
  pollTimer = 0;
}

function coverColor(name) {
  let hash = 0;
  for (const ch of name) hash = (hash * 33 + ch.codePointAt(0)) >>> 0;
  return covers[hash % covers.length];
}

function coverMonogram(game) {
  if (game.title_id) return game.title_id.slice(0, 4);
  const name = (game.name || "?").trim();
  return name.slice(0, 2).toUpperCase();
}

function displayName(game) {
  const name = (game.name || "").trim();
  const id = (game.title_id || "").trim();
  if (!id) return name || "未知目录";
  const catalog = (game.display_name || "").trim();
  if (catalog) return catalog;
  if (!name) return id;
  return name;
}

function shelfTitle(game) {
  return displayName(game);
}

function shelfSubtitle(game) {
  const name = (game.name || "").trim();
  const id = (game.title_id || "").trim();
  const parts = [];
  if (id) {
    parts.push(name && name.toUpperCase() !== id ? id : "Title ID");
  }
  const state = stateText[game.state] || game.state;
  if (state) parts.push(state);
  return parts.join(" · ");
}

function buildFallbackCover(game, art) {
  art.style.background = coverColor(game.name || game.title_id || "?");
  const mono = document.createElement("span");
  mono.className = "case-monogram";
  mono.textContent = coverMonogram(game);
  if (game.title_id && game.title_id.length > 4) {
    const code = document.createElement("span");
    code.className = "case-id";
    code.textContent = game.title_id.slice(4);
    art.append(mono, code);
  } else {
    art.append(mono);
  }
}

function buildCaseCover(game) {
  const art = document.createElement("div");
  art.className = "case-art";
  const coverUrl = (game.title_id || "").trim() ? (game.cover_url || "").trim() : "";
  if (coverUrl) {
    const img = document.createElement("img");
    img.className = "case-cover";
    img.alt = displayName(game);
    img.loading = "lazy";
    img.decoding = "async";
    img.src = coverUrl;
    img.addEventListener("error", () => {
      img.remove();
      art.classList.remove("has-cover");
      buildFallbackCover(game, art);
    }, { once: true });
    art.classList.add("has-cover");
    art.append(img);
  } else {
    buildFallbackCover(game, art);
  }
  return art;
}

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + " MB";
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + " GB";
}

async function api(path, body) {
  const response = await fetch(path, body === undefined ? undefined : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || "请求没有完成");
  return data;
}

function show(view) {
  if (view !== "shelf") closeDetail();
  shelfView.hidden = view !== "shelf";
  settingsView.hidden = view !== "settings";
}

function renderShelf() {
  shelf.replaceChildren();
  for (const game of pageGames()) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "case";
    const title = document.createElement("strong");
    title.className = "case-title";
    title.textContent = shelfTitle(game);
    const meta = document.createElement("span");
    meta.className = "case-meta";
    meta.textContent = shelfSubtitle(game);
    button.append(buildCaseCover(game), title, meta);
    button.addEventListener("click", () => renderDetail(game));
    shelf.append(button);
  }
  renderPagination();
}

function detailInfoLines(game) {
  const parts = [];
  const identified = Boolean((game.title_id || "").trim());
  if (identified) parts.push(game.title_id);
  else parts.push("目录名里没有 Title ID");
  if (identified && game.platform) parts.push(game.platform.toUpperCase());
  if (identified && game.region) parts.push("区域 " + game.region);
  if (identified && game.publisher_id) parts.push(game.publisher_id);
  if (game.formats) parts.push(game.formats);
  parts.push(formatBytes(game.archive_bytes || 0));
  if (!identified || game.catalog_matched === false) parts.push("资料库未收录");
  return parts.join(" · ");
}

function fillDetail(game, lines, action, meta) {
  lines.textContent = detailInfoLines(game);
  if (meta) {
    meta.replaceChildren();
    const folder = (game.name || "").trim();
    if (folder && folder !== displayName(game)) {
      const folderLine = document.createElement("p");
      folderLine.className = "detail-folder";
      folderLine.textContent = "网盘目录：" + folder;
      meta.append(folderLine);
    }
    if ((game.title_id || "").trim() && game.concept_id) {
      const conceptLine = document.createElement("p");
      conceptLine.className = "detail-concept";
      conceptLine.textContent = "Concept ID " + game.concept_id;
      meta.append(conceptLine);
    }
    if (game.catalog_matched === false) {
      const note = document.createElement("p");
      note.className = "detail-note";
      note.textContent = "本地资料库没有收录这款游戏，仍可按目录名安装。";
      meta.append(note);
    }
  }
  action.className = "action" + (game.state === "installed" ? " done" : "");
  action.textContent = stateText[game.state] || "安装";
  action.disabled = game.state === "installed" || game.state === "installing" || game.state === "unsupported" || game.state === "empty" || game.state === "need-settings" || !game.formats;
}

function closeDetail() {
  if (detailOverlay.hidden) return;
  detailOverlay.hidden = true;
  document.body.classList.remove("detail-open");
  if (detailEscapeHandler) {
    document.removeEventListener("keydown", detailEscapeHandler);
    detailEscapeHandler = null;
  }
}

function openDetail() {
  detailOverlay.hidden = false;
  document.body.classList.add("detail-open");
  if (!detailEscapeHandler) {
    detailEscapeHandler = (event) => {
      if (event.key === "Escape") closeDetail();
    };
    document.addEventListener("keydown", detailEscapeHandler);
  }
  detailClose.focus();
}

function renderDetail(game) {
  openDetail();
  detailBody.replaceChildren();
  const cover = document.createElement("div");
  cover.className = "case";
  const title = document.createElement("strong");
  title.className = "case-title";
  title.textContent = shelfTitle(game);
  cover.append(buildCaseCover(game), title);
  const copy = document.createElement("div");
  const lines = document.createElement("p");
  const meta = document.createElement("div");
  meta.className = "detail-meta";
  const action = document.createElement("button");
  action.type = "button";
  fillDetail(game, lines, action, meta);
  const progress = document.createElement("p");
  progress.id = "job-progress";
  const refresh = () => fillDetail(game, lines, action, meta);
  action.addEventListener("click", () => startInstall(game, progress, refresh));
  copy.append(lines, meta, action, progress);
  detailBody.append(cover, copy);
  api("/api/game", { path: game.path }).then(async (data) => {
    Object.assign(game, data);
    title.textContent = displayName(game);
    const newCover = buildCaseCover(game);
    const oldArt = cover.querySelector(".case-art");
    if (oldArt) oldArt.replaceWith(newCover);
    let job = null;
    try {
      job = await api("/api/job");
    } catch (error) {
      job = null;
    }
    if (job && job.path === game.path && (job.running || job.status)) {
      game.state = job.running ? "installing" : settledState(job);
      progress.textContent = jobProgressText(job);
    }
    refresh();
    if (job && job.running && job.path === game.path) watchJob(progress, game, refresh);
  }).catch((error) => {
    lines.textContent = error.message;
  });
}

function jobProgressText(job) {
  if (job.running) {
    if (job.files_total > 0) return "正在安装 " + job.files_done + " / " + job.files_total;
    return job.message || "正在读取压缩包…";
  }
  if (job.message) return job.message;
  if (job.status === "installed" || job.status === "already") return "安装完成";
  return job.status || "";
}

function settledState(job) {
  if (job.status === "installed" || job.status === "already") return "installed";
  if (job.status === "interrupted") return "continue";
  return "install";
}

function watchJob(progress, game, refresh) {
  const tick = async () => {
    let job;
    try {
      job = await api("/api/job");
    } catch (error) {
      return;
    }
    if (!job.path || job.path !== game.path) return;
    progress.textContent = jobProgressText(job);
    if (job.running) {
      if (game.state !== "installing") {
        game.state = "installing";
        refresh();
      }
      return;
    }
    window.clearInterval(jobTimer);
    jobTimer = 0;
    game.state = settledState(job);
    refresh();
    loadGames();
  };
  window.clearInterval(jobTimer);
  jobTimer = window.setInterval(tick, 1000);
  tick();
}

async function startInstall(game, progress, refresh) {
  progress.textContent = "正在读取压缩包…";
  try {
    await api("/api/install", { path: game.path });
  } catch (error) {
    progress.textContent = error.message;
    return;
  }
  game.state = "installing";
  refresh();
  watchJob(progress, game, refresh);
}

const loginButton = document.querySelector("#start-login");
let loggedIn = false;

function setLoggedIn(on) {
  loggedIn = on;
  loginButton.hidden = on;
  loginBox.hidden = on && !loginFlowActive();
}

function updateLede() {
  if (!loggedIn || loginFlowActive()) return;
  if (games.length === 0) {
    lede.textContent = shelfStatus();
    return;
  }
  const hint = shelfStatus().replace(/^已经登录。/, "").trim();
  lede.textContent = games.length + " 个目录" + (hint ? " · " + hint : "");
}

function shelfStatus() {
  if (games.length === 0) return "这个目录里还没有游戏文件夹。";
  if (games.every((game) => game.state === "need-settings")) return "已经登录。到设置里填写存放位置后才能安装。";
  return "已经登录。点开封面查看详情和安装状态。";
}

async function loadGames() {
  try {
    const data = await api("/api/games");
    games = data.games || [];
    currentPage = 1;
    setLoggedIn(true);
    if (!loginFlowActive()) loginText.textContent = shelfStatus();
    updateLede();
    renderShelf();
  } catch (error) {
    if (loginFlowActive()) return;
    if (error.message === "百度未登录") {
      setLoggedIn(false);
      games = [];
      currentPage = 1;
      renderShelf();
      loginText.textContent = signedOutHint;
      return;
    }
    if (loggedIn) lede.textContent = loginErrorText(error.message);
    else loginText.textContent = loginErrorText(error.message);
  }
}

function finishLogin() {
  stopLoginPoll();
  loginText.textContent = "已经登录。";
  qr.hidden = true;
  userCode.textContent = "";
  loadGames();
}

async function pollLogin() {
  pollTimer = 0;
  try {
    const poll = await api("/api/login/poll", {});
    if (poll.done) {
      finishLogin();
      return;
    }
    if (poll.expired || poll.error) {
      stopLoginPoll();
      loginText.textContent = loginErrorText(poll.error || "二维码过期了，再显示一次。");
      return;
    }
    const wait = Math.max(5, poll.retry_after || 5);
    loginText.textContent = "手机上确认之后，页面会自动进入游戏目录。";
    pollTimer = window.setTimeout(() => { pollLogin(); }, wait * 1000);
  } catch (error) {
    stopLoginPoll();
    loginText.textContent = loginErrorText(error.message);
  }
}

async function startLogin() {
  stopLoginPoll();
  loginBox.hidden = false;
  loginButton.disabled = true;
  loginText.textContent = "正在向百度获取二维码…";
  let challenge;
  try {
    challenge = await api("/api/login/start", {});
  } finally {
    loginButton.disabled = false;
  }
  if (challenge.already) {
    setLoggedIn(true);
    qr.hidden = true;
    loginText.textContent = "已经登录，正在读取游戏目录…";
    await loadGames();
    return;
  }
  if (challenge.qrcode_url) {
    qr.hidden = false;
    qr.src = challenge.qrcode_url;
    qr.alt = "百度登录二维码";
  }
  userCode.textContent = challenge.user_code ? "用户码 " + challenge.user_code : "";
  loginText.textContent = "用百度 App、百度网盘或微信扫描。确认后会自动进入。";
  pollTimer = window.setTimeout(() => { pollLogin(); }, Math.max(5, challenge.interval || 5) * 1000);
}

function formatCatalogTime(unixSeconds) {
  if (!unixSeconds) return "未知";
  const date = new Date(unixSeconds * 1000);
  if (Number.isNaN(date.getTime())) return "未知";
  return date.toLocaleString();
}

function renderCatalogStatus(info) {
  const sync = info && info.sync ? info.sync : {};
  if (sync.phase === "updating") {
    catalogStatus.textContent = "游戏库数据：正在后台同步…";
    return;
  }
  if (!info || !info.loaded) {
    catalogStatus.textContent = "游戏库数据：尚未就绪，后台正在首次同步（界面仍可用目录名）。";
    return;
  }
  const parts = [`游戏库数据：已同步 ${info.count.toLocaleString()} 款`];
  if (info.generated_at) parts.push("上次更新 " + info.generated_at.replace("T", " ").replace("Z", " UTC"));
  else if (info.file_mtime) parts.push("上次更新 " + formatCatalogTime(info.file_mtime));
  else if (sync.last_success) parts.push("上次更新 " + formatCatalogTime(sync.last_success));
  if (info.stale) parts.push("已超过 " + (sync.stale_days || 7) + " 天，将自动重新同步");
  if (sync.last_error) parts.push("最近同步失败：" + sync.last_error);
  catalogStatus.textContent = parts.join(" · ");
}

let catalogPollTimer = null;

function stopCatalogPoll() {
  if (catalogPollTimer) {
    clearInterval(catalogPollTimer);
    catalogPollTimer = null;
  }
}

async function loadCatalogStatus() {
  const info = await api("/api/catalog");
  renderCatalogStatus(info);
  stopCatalogPoll();
  if (info && info.sync && info.sync.phase === "updating") {
    catalogPollTimer = setInterval(() => {
      loadCatalogStatus().catch(() => {});
    }, 5000);
  }
}

async function loadSettings() {
  const settings = await api("/api/settings");
  for (const name of ["app_key", "secret_key", "library_path", "dest", "prefix"]) {
    form.elements[name].value = settings[name] || "";
  }
  await loadCatalogStatus();
}

detailBackdrop.addEventListener("click", closeDetail);
detailClose.addEventListener("click", closeDetail);
detail.addEventListener("click", (event) => event.stopPropagation());

document.querySelector("#show-shelf").addEventListener("click", () => show("shelf"));
shelfPrev.addEventListener("click", () => goToPage(currentPage - 1));
shelfNext.addEventListener("click", () => goToPage(currentPage + 1));
document.querySelector("#show-settings").addEventListener("click", () => {
  show("settings");
  loadSettings().catch((error) => { settingsNote.textContent = error.message; });
});
catalogReload.addEventListener("click", async () => {
  catalogReload.disabled = true;
  catalogNote.textContent = "正在重读资料库…";
  try {
    const info = await api("/api/catalog/reload", {});
    renderCatalogStatus(info);
    catalogNote.textContent = "已从磁盘重新加载。";
    if (loggedIn) await loadGames();
  } catch (error) {
    catalogNote.textContent = error.message;
  } finally {
    catalogReload.disabled = false;
  }
});
document.querySelector("#start-login").addEventListener("click", () => {
  startLogin().catch((error) => { loginText.textContent = loginErrorText(error.message); });
});
form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const body = {};
  for (const name of ["app_key", "secret_key", "library_path", "dest", "prefix"]) body[name] = form.elements[name].value;
  try {
    await api("/api/settings", body);
    settingsNote.textContent = "已保存在这台电脑上。";
  } catch (error) {
    settingsNote.textContent = error.message;
  }
});

async function boot() {
  loginText.textContent = "正在确认登录…";
  loginButton.disabled = true;
  try {
    const session = await api("/api/session");
    setLoggedIn(!!session.logged_in);
    if (!session.logged_in) {
      loginBox.hidden = false;
      loginText.textContent = signedOutHint;
      loginButton.disabled = false;
      return;
    }
    setLoggedIn(true);
    lede.textContent = "正在读取游戏目录…";
    loginText.textContent = "已经登录，正在读取游戏目录…";
    await loadGames();
  } catch (error) {
    loginButton.disabled = false;
    if (loggedIn) lede.textContent = loginErrorText(error.message);
    else loginText.textContent = loginErrorText(error.message);
  }
}

loadSettings().catch(() => {});
boot();
