const $ = (selector) => document.querySelector(selector);

const shelfView = $("#shelf-view");
const settingsView = $("#settings-view");
const library = $("#library");
const shelf = $("#shelf");
const shelfEmpty = $("#shelf-empty");
const shelfMore = $("#shelf-more");
const shelfEnd = $("#shelf-end");
const gameCount = $("#game-count");
const lede = $("#lede");
const search = $("#search");
const filterButtons = document.querySelectorAll(".filter");
const refineButtons = document.querySelectorAll("[data-refine]");
const sortSelect = $("#sort");
const loginBox = $("#login-box");
const loginText = $("#login-text");
const loginButton = $("#start-login");
const qr = $("#qr");
const qrWrap = $("#qr-wrap");
const userCode = $("#user-code");
const detailOverlay = $("#detail-overlay");
const detailBody = $("#detail-body");
const dock = $("#dock");
const dockArt = $("#dock-art");
const dockTitle = $("#dock-title");
const dockStatus = $("#dock-status");
const dockMeter = $("#dock-meter");
const dockAction = $("#dock-action");
const dockToggle = $("#dock-toggle");
const dockPanel = $("#dock-panel");
const dockClear = $("#dock-clear");
const queueList = $("#queue-list");
const form = $("#settings-form");
const settingsNote = $("#settings-note");
const catalogStatus = $("#catalog-status");
const catalogNote = $("#catalog-note");
const catalogReload = $("#catalog-reload");

const signedOutHint = "登录后才能看到网盘里的游戏。";
const regionText = { UP: "美服", EP: "欧服", JP: "日服", HP: "港服", KP: "韩服", IP: "国际服" };
const storeLocaleText = { "zh-hans-hk": "港服商店（简体）", "zh-hant-hk": "港服商店（繁体）", "en-us": "美服商店" };
const metaStateNote = {
  pending: "正在从商店补全中文名和封面…",
  missing: "商店里没有找到这款游戏，可以在下面手动改名。",
  offline: "商店暂时连不上，稍后会自动重试。",
};

const langNames = {
  zh: "中文", ch: "中文", en: "英语", ja: "日语", ko: "韩语", fr: "法语", de: "德语", es: "西班牙语", it: "意大利语",
  pt: "葡萄牙语", ru: "俄语", ar: "阿拉伯语", pl: "波兰语", nl: "荷兰语", th: "泰语", tr: "土耳其语", sv: "瑞典语",
  da: "丹麦语", fi: "芬兰语", no: "挪威语", cs: "捷克语", hu: "匈牙利语", el: "希腊语", id: "印尼语", vi: "越南语", uk: "乌克兰语",
};
const classificationText = { GAME_BUNDLE: "捆绑包", PREMIUM_EDITION: "高级版", ADD_ON_PACK: "附加内容", DEMO: "试玩版" };

let games = [];
let filter = "all";
// 按商店资料筛选：cn 只看有中文的，multi 只看多人同屏的；sortBy 为空时按游戏库目录顺序。
const refine = { cn: false, multi: false };
let sortBy = "";
let query = "";
let searchTimer = 0;
// 游戏库按页从服务器取：loaded 是已经取到的位置，total 是符合当前筛选的总数，counts 是整个游戏库的数量。
let loaded = 0;
let total = 0;
let counts = { all: 0, installed: 0, need_settings: false };
let pageToken = 0;
let pageLoading = false;
let loggedIn = false;
let loginTimer = 0;
let metaTimer = 0;
let metaRounds = 0;
let tasks = [];
let queueTimer = 0;
let dockOpen = false;
let detail = null;
const tiles = new Map();

// 开了访问口令时，别的设备要先输口令；同时等口令的请求共用一次输入。
let unlocking = null;

function unlock() {
  if (unlocking) return unlocking;
  const box = $("#unlock-box");
  const form = $("#unlock-form");
  const note = $("#unlock-note");
  box.hidden = false;
  form.elements.code.focus();
  unlocking = new Promise((resolve) => {
    form.onsubmit = async (event) => {
      event.preventDefault();
      note.textContent = "";
      const response = await fetch("/api/unlock", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ code: form.elements.code.value.trim() }),
      }).catch(() => null);
      const data = response ? await response.json().catch(() => ({})) : {};
      if (!response || !response.ok) {
        note.textContent = data.error || "没有连上主机";
        return;
      }
      box.hidden = true;
      form.reset();
      unlocking = null;
      resolve();
    };
  });
  return unlocking;
}

async function api(path, body) {
  const send = () => fetch(path, body === undefined ? undefined : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  let response = await send();
  let data = await response.json().catch(() => ({}));
  if (response.status === 401 && data.locked) {
    await unlock();
    response = await send();
    data = await response.json().catch(() => ({}));
  }
  if (!response.ok) throw new Error(data.error || "请求没有完成");
  return data;
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function displayName(game) {
  return (game.display_name || "").trim() || (game.name || "").trim() || (game.title_id || "").trim() || "未知目录";
}

function platformText(game) {
  if (game.platform) return game.platform.toUpperCase();
  const id = (game.title_id || "").toUpperCase();
  if (id.startsWith("PPSA")) return "PS5";
  if (id.startsWith("CUSA")) return "PS4";
  return "";
}

function formatBytes(bytes) {
  if (!bytes) return "";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + " MB";
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + " GB";
}

function hashHue(text) {
  let hash = 0;
  for (const ch of text) hash = (hash * 33 + ch.codePointAt(0)) >>> 0;
  return hash % 360;
}

// 撑尺寸用的透明正方形图。PS5 自带浏览器里格子的宽高跟着里面的图走，没有图就缩成 0，
// 所以每个格子都垫一张，封面和占位绝对定位盖在上面，和有封面时的排版一模一样。
const ART_SIZER = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='1000' height='1000'/%3E";

// 封面：有商店图用商店图，没有就用 Title ID 生成一张。
function buildArt(game, className) {
  const art = el("span", "art " + (className || ""));
  const sizer = document.createElement("img");
  sizer.className = "art-sizer";
  sizer.alt = "";
  sizer.src = ART_SIZER;
  art.append(sizer);
  const fallback = () => {
    const hue = hashHue(game.title_id || game.name || "?");
    art.style.background = `linear-gradient(160deg, hsl(${hue} 42% 34%), hsl(${(hue + 40) % 360} 46% 18%))`;
    const box = el("span", "fallback");
    const id = (game.title_id || "").trim();
    if (id) {
      box.append(el("span", "fallback-kind", id.slice(0, 4)), el("span", "fallback-code", id.slice(4)));
    } else {
      box.append(el("span", "fallback-code", (game.name || "?").trim().slice(0, 2)));
    }
    art.append(box);
  };
  const url = (game.cover_url || "").trim();
  if (url) {
    const img = document.createElement("img");
    img.className = "cover";
    img.alt = "";
    img.loading = "lazy";
    img.decoding = "async";
    img.src = url;
    img.addEventListener("error", () => {
      img.remove();
      fallback();
    }, { once: true });
    art.append(img);
  } else {
    fallback();
  }
  const fill = el("span", "fill");
  fill.append(el("span"));
  art.append(fill);
  return art;
}

/* 安装队列：服务器一次装一个，tasks 依次是正在装的、排队的、最近完成的 */

function taskFor(path) {
  return tasks.find((task) => task.path === path) || null;
}

function runningTask() {
  return tasks.find((task) => task.state === "running") || null;
}

function queuedTasks() {
  return tasks.filter((task) => task.state === "queued");
}

function queuePosition(path) {
  return queuedTasks().findIndex((task) => task.path === path) + 1;
}

function queueBusy() {
  return tasks.some((task) => task.state !== "done");
}

function taskFraction(task) {
  if (!task) return 0;
  if (task.bytes_total) return Math.min(1, task.bytes_done / task.bytes_total);
  if (!task.files_total) return 0;
  return Math.min(1, task.files_done / task.files_total);
}

function taskStarted(task) {
  return Boolean(task && (task.bytes_total || task.files_total));
}

function jobFor(game) {
  const task = taskFor(game.path);
  return task && task.state === "running" ? task : null;
}

function paintArt(art, game) {
  art.dataset.state = game.state;
  const task = jobFor(game);
  art.style.setProperty("--p", taskFraction(task));
  const label = art.querySelector(".fill span");
  if (label) label.textContent = taskStarted(task) ? Math.round(taskFraction(task) * 100) + "%" : "…";
  art.querySelector(".badge")?.remove();
  if (game.state === "installed") art.append(el("span", "badge installed", "已安装"));
  if (game.state === "continue") art.append(el("span", "badge continue", "未装完"));
  if (game.state === "update") art.append(el("span", "badge continue", "有更新"));
  if (game.state === "queued") art.append(el("span", "badge queued num", "排队 " + (queuePosition(game.path) || "")));
  // 自己评价过这个版本：左下角标出能不能玩。
  art.querySelector(".review-mark")?.remove();
  if (game.review_state === "good") art.append(el("span", "review-mark good", "能玩"));
  if (game.review_state === "bad") art.append(el("span", "review-mark bad", "有问题"));
}

/* 游戏库 */

function renderCounts() {
  gameCount.textContent = counts.all ? String(counts.all) : "";
  shelfEmpty.hidden = loaded > 0;
  if (!counts.all) shelfEmpty.textContent = "游戏库目录里还没有游戏文件夹。";
  else if (query) shelfEmpty.textContent = `没有找到“${search.value.trim()}”。`;
  else if (refine.cn || refine.multi) shelfEmpty.textContent = "没有符合筛选的游戏。商店资料还在补全时，稍后会多一些。";
  else if (filter === "installed") shelfEmpty.textContent = "还没有装到主机上的游戏。";
  else if (filter === "continue") shelfEmpty.textContent = "没有装到一半的游戏。";
  else shelfEmpty.textContent = "这里没有游戏了。";
  updateLede();
}

// 当前窗口放得下几个游戏：按书架实际的列数和行高算，再多一行，好让页面滚得起来。
function pageSize() {
  const style = getComputedStyle(shelf);
  const columns = style.gridTemplateColumns.split(" ").filter((part) => part.endsWith("px"));
  const perRow = columns.length || Math.max(1, Math.floor((window.innerWidth - 64) / 186));
  const sample = shelf.firstElementChild;
  const tileHeight = sample ? sample.getBoundingClientRect().height : (parseFloat(columns[0]) || 164) + 70;
  const rowHeight = tileHeight + (parseFloat(style.rowGap) || 32);
  const top = Math.min(Math.max(0, shelf.getBoundingClientRect().top), window.innerHeight);
  const rows = Math.ceil((window.innerHeight - top) / rowHeight) + 1;
  return perRow * Math.max(2, rows);
}

// 取一页游戏。reset 时从头取，替换掉已有的格子；fresh 让服务器重新列网盘目录、重读主机上的安装情况。
async function fetchShelf({ reset = false, fresh = false } = {}) {
  if (!reset && (pageLoading || loaded >= total)) return;
  const token = reset ? ++pageToken : pageToken;
  pageLoading = true;
  shelfMore.hidden = reset;
  shelfMore.textContent = "正在加载更多…";
  const limit = reset && fresh ? Math.max(loaded, pageSize()) : pageSize();
  const params = new URLSearchParams({ offset: reset ? 0 : loaded, limit });
  if (filter !== "all") params.set("filter", filter);
  if (query) params.set("q", query);
  if (refine.cn) params.set("cn", "1");
  if (refine.multi) params.set("multi", "1");
  if (sortBy) params.set("sort", sortBy);
  if (fresh) params.set("fresh", "1");
  try {
    const data = await api("/api/games?" + params);
    if (token !== pageToken) return;
    const page = data.games || [];
    for (const game of page) syncGameState(game);
    if (reset) {
      games = [];
      tiles.clear();
      shelf.replaceChildren();
    }
    for (const game of page) {
      if (tiles.has(game.path)) continue;
      games.push(game);
      shelf.append(buildTile(game));
    }
    loaded = (data.offset || 0) + page.length;
    total = data.total || 0;
    counts = { all: data.all || 0, installed: data.installed || 0, need_settings: Boolean(data.need_settings) };
    renderCounts();
    scheduleMetaPoll(reset);
    window.requestAnimationFrame(fillViewport);
  } catch (error) {
    if (token !== pageToken) return;
    if (reset) throw error;
    shelfMore.hidden = false;
    shelfMore.textContent = "没有加载出来：" + error.message;
  } finally {
    if (token === pageToken) {
      pageLoading = false;
      if (shelfMore.textContent === "正在加载更多…") shelfMore.hidden = true;
    }
  }
}

// 书架底部离窗口不远了就接着取下一页，直到填满窗口或者全部取完。
function fillViewport() {
  if (library.hidden || shelfView.hidden || pageLoading || loaded >= total) return;
  if (shelfEnd.getBoundingClientRect().top < window.innerHeight + 600) fetchShelf();
}

new IntersectionObserver((entries) => {
  if (entries.some((entry) => entry.isIntersecting)) fillViewport();
}, { rootMargin: "0px 0px 600px 0px" }).observe(shelfEnd);
window.addEventListener("resize", fillViewport);

function reloadShelf() {
  fetchShelf({ reset: true }).catch((error) => { lede.textContent = error.message; });
}

function refreshTile(game) {
  const entry = tiles.get(game.path);
  if (entry) entry.tile.replaceWith(buildTile(game));
}

function buildTile(game) {
  const tile = el("button", "tile");
  tile.type = "button";
  const art = buildArt(game);
  const sub = el("span", "tile-sub");
  const platform = platformText(game);
  if (platform) sub.append(el("span", "plat", platform));
  if (game.title_id) sub.append(el("span", "num", game.title_id));
  if (game.has_cn_sub || game.has_cn_voice) {
    const cn = el("span", "tile-cn", "中");
    cn.title = game.has_cn_voice ? "有中文语音" : "有中文字幕";
    sub.append(cn);
  }
  if (game.rating != null) {
    const rating = el("span", "tile-rating num", "★ " + Number(game.rating).toFixed(1));
    rating.title = `PlayStation 商店评分 ${game.rating}（${game.rating_count} 人评分）`;
    sub.append(rating);
  }
  tile.append(art, el("span", "tile-name", displayName(game)), sub);
  tile.addEventListener("click", () => openDetail(game, tile));
  tiles.set(game.path, { tile, art });
  paintTile(game);
  return tile;
}

function paintTile(game) {
  const entry = tiles.get(game.path);
  if (!entry) return;
  entry.tile.dataset.state = game.state;
  paintArt(entry.art, game);
}

function updateLede() {
  lede.replaceChildren();
  if (counts.all && counts.need_settings) {
    const go = el("button", "", "到设置里填写存放位置");
    go.type = "button";
    go.addEventListener("click", () => show("settings", "target"));
    lede.append(go, "后才能安装。");
    return;
  }
  if (counts.all) lede.textContent = counts.installed ? `已装到主机 ${counts.installed} 个。` : "点开游戏就能装到主机。";
}

async function loadGames() {
  try {
    await fetchShelf({ reset: true, fresh: true });
    setLoggedIn(true);
  } catch (error) {
    if (loginFlowActive()) return;
    if (error.message === "百度未登录") {
      games = [];
      loaded = 0;
      total = 0;
      setLoggedIn(false);
      loginText.textContent = signedOutHint;
      return;
    }
    if (loggedIn) lede.textContent = error.message;
    else loginText.textContent = error.message;
  }
}

const sharedMetaFields = [
  "display_name", "name_source", "store_locale", "catalog_name", "cover_url", "meta_state",
  "rating", "rating_count", "has_cn_sub", "has_cn_voice", "players",
];

function shareMeta(source) {
  const sameGame = (game) => (source.title_id ? game.title_id === source.title_id : game.name === source.name);
  for (const game of games) {
    if (!sameGame(game)) continue;
    if (game !== source) for (const field of sharedMetaFields) game[field] = source[field];
    refreshTile(game);
  }
}

function scheduleMetaPoll(reset) {
  if (reset) metaRounds = 0;
  window.clearTimeout(metaTimer);
  metaTimer = 0;
  const waiting = games.some((game) => game.meta_state === "pending") || (detail && detail.game.meta_state === "pending");
  if (!waiting || metaRounds >= 200) return;
  metaTimer = window.setTimeout(pollMeta, 3000);
}

async function pollMeta() {
  metaTimer = 0;
  metaRounds += 1;
  const pending = games.filter((game) => game.meta_state === "pending");
  if (detail && detail.game.meta_state === "pending" && !pending.includes(detail.game)) pending.push(detail.game);
  try {
    const data = await api("/api/meta", { names: pending.map((game) => game.name) });
    (data.items || []).forEach((item, index) => {
      const game = pending[index];
      if (!game || game.name !== item.name) return;
      const changed = item.meta_state !== game.meta_state || item.display_name !== game.display_name ||
        item.cover_url !== game.cover_url || item.rating !== game.rating ||
        item.has_cn_sub !== game.has_cn_sub || item.has_cn_voice !== game.has_cn_voice;
      Object.assign(game, item);
      if (!changed) return;
      refreshTile(game);
      if (detail && detail.game === game) detail.reload();
    });
  } catch (error) {
    // 下一轮再试
  }
  scheduleMetaPoll(false);
}

function formatDuration(seconds) {
  const minutes = Math.round(seconds / 60);
  if (minutes < 1) return "不到 1 分钟";
  if (minutes < 60) return `${minutes} 分钟`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return minutes % 60 ? `${hours} 小时 ${minutes % 60} 分` : `${hours} 小时`;
  return hours % 24 ? `${Math.floor(hours / 24)} 天 ${hours % 24} 小时` : `${Math.floor(hours / 24)} 天`;
}

// 预计几点装完：今天只写时刻，之后的写上哪天。
// 运行盘游那台机器的时区比 UTC 快多少分钟，队列接口带回来。PS5 自带浏览器拿不到时区、按 UTC 算，
// 所以钟点按它算；没拿到时退回浏览器自己的时区。
let serverUtcOffset = null;

// 按那台机器的时区平移，之后用 getUTC* 取到的就是那里的日期和钟点。
function serverClockDate(ms) {
  const offset = serverUtcOffset ?? -new Date().getTimezoneOffset();
  return new Date(ms + offset * 60000);
}

const pad2 = (value) => String(value).padStart(2, "0");

function finishClock(seconds) {
  const at = serverClockDate(Date.now() + seconds * 1000);
  const now = serverClockDate(Date.now());
  const clock = `${pad2(at.getUTCHours())}:${pad2(at.getUTCMinutes())}`;
  const dayNumber = (date) => Math.floor(date.getTime() / 86400000);
  const days = dayNumber(at) - dayNumber(now);
  if (days === 0) return clock;
  if (days === 1) return `明天 ${clock}`;
  if (days === 2) return `后天 ${clock}`;
  return `${at.getUTCMonth() + 1} 月 ${at.getUTCDate()} 日 ${clock}`;
}

// 正在解压时的平均速度和剩余时间；刚开始几秒还没算出来时没有。
function taskPace(task) {
  if (!task || task.state !== "running" || task.message !== "正在安装" || !(task.speed > 0)) return null;
  return { speed: formatBytes(task.speed) + "/s", left: task.eta >= 0 ? task.eta : null };
}

// inDetail：详情页上方已经有百分比和预计几点装完，这一行只写进度和速度；底部进度栏没有，写剩余时间。
// 文字要短：PS5 自带浏览器里这行太长会把整个抽屉撑宽，每秒变一次就跟着晃。
function jobText(current, inDetail = false) {
  if (current.state === "queued") return `排队中，第 ${queuePosition(current.path)} 个`;
  if (current.state === "running") {
    if (current.message === "正在取消…" || current.message === "正在注册到主机桌面…" ||
      (current.message || "").startsWith("安装中断")) return current.message;
    if (current.message === "正在收尾") return `正在收尾，共 ${current.files_total} 个文件`;
    if (current.message === "正在核对") return `核对已装文件 ${current.files_checked} / ${current.files_to_check}`;
    if (current.bytes_total > 0 && current.message === "正在安装") {
      const parts = [`${formatBytes(current.bytes_done)} / ${formatBytes(current.bytes_total)}`];
      const pace = taskPace(current);
      if (inDetail && pace) parts.push(pace.speed);
      else if (!inDetail && pace && pace.left !== null) parts.push(`剩余 ${formatDuration(pace.left)}`);
      else parts.push(`${current.files_done} 个文件`);
      return parts.join(" · ");
    }
    if (current.files_total > 0) return `已装 ${current.files_done} / ${current.files_total} 个文件`;
    return current.message || "正在读取压缩包…";
  }
  if (current.status === "installed" && current.speed > 0 && current.elapsed > 0) {
    return `${current.message || "安装完成"} · 用时 ${formatDuration(current.elapsed)} · 平均 ${formatBytes(current.speed)}/s`;
  }
  if (current.message) return current.message;
  if (current.status === "installed" || current.status === "already") return "安装完成";
  return current.status || "";
}

function settledState(current) {
  if (current.status === "installed" || current.status === "already") return "installed";
  if (current.status === "interrupted") return "continue";
  return "install";
}

// 游戏的状态跟着队列走；离开队列的游戏按安装结果定，等下次刷新游戏库再以主机上的为准。
function syncGameState(game) {
  const task = taskFor(game.path);
  if (task && task.state === "running") game.state = "installing";
  else if (task && task.state === "queued") game.state = "queued";
  else if (game.state === "installing" || game.state === "queued") game.state = task ? settledState(task) : "install";
}

// 详情页读到的压缩包和安装情况同步给列表里同一款游戏，格子上的角标跟着变。
function shareDetail(source) {
  for (const game of games) {
    if (game.path !== source.path) continue;
    if (game !== source) {
      for (const field of ["state", "archive_bytes", "formats", "layers", "review_state"]) game[field] = source[field];
    }
    syncGameState(game);
    paintTile(game);
  }
}

// 列表刷新后详情里拿的可能是旧对象，所以详情里的游戏单独更新。
function applyQueue() {
  for (const game of games) {
    syncGameState(game);
    paintTile(game);
  }
  if (detail) {
    syncGameState(detail.game);
    detail.paint();
  }
  paintDock();
}

function setQueue(data) {
  const active = new Set(tasks.filter((task) => task.state !== "done").map((task) => task.path));
  tasks = data.tasks || [];
  if (Number.isFinite(data.utc_offset_min)) serverUtcOffset = data.utc_offset_min;
  const settled = tasks.filter((task) => task.state === "done" && active.has(task.path)).map((task) => task.path);
  applyQueue();
  if (settled.length) loadGames();
  if (detail && settled.includes(detail.game.path)) detail.reload();
  if (queueBusy()) watchQueue();
  else stopQueueWatch();
}

async function refreshQueue() {
  try {
    setQueue(await api("/api/queue"));
  } catch (error) {
    // 下一秒再试
  }
}

function watchQueue() {
  if (!queueTimer) queueTimer = window.setInterval(refreshQueue, 1000);
}

function stopQueueWatch() {
  window.clearInterval(queueTimer);
  queueTimer = 0;
}

// 加入队列、移出、排到下一个、清除都返回新的队列。
async function queueAction(path, body) {
  try {
    setQueue(await api(path, body));
    return "";
  } catch (error) {
    return error.message;
  }
}

// endpoint 是 /api/discard（删掉没装完的文件）或 /api/uninstall（删掉装好的整个游戏）。成功返回空，失败返回原因。
async function removeInstall(game, endpoint) {
  const error = await queueAction(endpoint, { path: game.path });
  if (error) return error;
  for (const item of games) {
    if (item.path !== game.path) continue;
    item.state = "install";
    paintTile(item);
  }
  game.state = "install";
  for (const layer of game.layers || []) layer.installed = false;
  loadGames();
  return "";
}

async function discardInstall(game) {
  if (!window.confirm(`删除「${displayName(game)}」已经装了一部分的文件？\n删除后再装要从头开始。`)) return "cancelled";
  return removeInstall(game, "/api/discard");
}

// skip 是选了不装的压缩包组；不传时沿用上次的选择。
async function startInstall(game, skip) {
  const error = await queueAction("/api/install", skip ? { path: game.path, skip } : { path: game.path });
  if (error && detail && detail.game === game) detail.fail(error);
}

function openTask(task) {
  const game = games.find((item) => item.path === task.path);
  if (game) {
    openDetail(game);
    return;
  }
  const stub = { ...task, state: "install" };
  syncGameState(stub);
  openDetail(stub);
}

/* 底部的安装队列：收起时只显示正在装的，展开后列出整个队列 */

let dockArtKey = null;
const queueRows = new Map();

function dockSummary() {
  const running = runningTask();
  const queued = queuedTasks();
  const done = tasks.filter((task) => task.state === "done");
  const waitText = queued.length ? ` · 还有 ${queued.length} 个排队` : "";
  if (running) return { head: running, state: "installing", text: jobText(running) + waitText, p: taskFraction(running) };
  if (queued.length) return { head: queued[0], state: "installing", text: "准备安装下一个" + waitText, p: 0 };
  const failed = done.filter((task) => settledState(task) !== "installed").length;
  let text = jobText(done[0]);
  if (done.length > 1) text = failed ? `装好 ${done.length - failed} 个，${failed} 个没装完` : `${done.length} 个游戏都装好了`;
  return { head: done[0], state: failed ? "error" : "installed", text, p: 1 };
}

function paintDock() {
  dock.hidden = tasks.length === 0;
  if (dock.hidden) {
    dockOpen = false;
    return;
  }
  const summary = dockSummary();
  const running = runningTask();
  dock.dataset.state = summary.state;
  dockTitle.textContent = displayName(summary.head);
  dockStatus.textContent = summary.text;
  const pace = taskPace(running);
  dockStatus.title = pace && pace.left !== null ? `平均 ${pace.speed}，预计 ${finishClock(pace.left)} 装完` : "";
  const artKey = summary.head.path + "\n" + (summary.head.cover_url || "");
  if (dockArtKey !== artKey) {
    dockArtKey = artKey;
    dockArt.replaceChildren(buildArt(summary.head));
  }
  dockMeter.style.setProperty("--p", summary.p);
  dockAction.hidden = !running && queueBusy();
  dockAction.textContent = running ? "取消" : "关闭";
  dockAction.disabled = Boolean(running && running.message === "正在取消…");
  dockToggle.setAttribute("aria-expanded", String(dockOpen));
  dockToggle.setAttribute("aria-label", dockOpen ? "收起安装队列" : "展开安装队列");
  dockPanel.hidden = !dockOpen;
  dockClear.hidden = !tasks.some((task) => task.state === "done");
  if (dockOpen) paintQueueList();
}

function queueButton(text, onClick, className) {
  const button = el("button", "queue-btn" + (className ? " " + className : ""), text);
  button.type = "button";
  button.addEventListener("click", onClick);
  return button;
}

function queueRowActions(task) {
  const remove = () => queueAction("/api/queue/remove", { path: task.path });
  if (task.state === "running") {
    const cancel = queueButton("取消", remove, "danger");
    cancel.disabled = task.message === "正在取消…";
    return [cancel];
  }
  if (task.state === "queued") {
    const buttons = [];
    if (queuePosition(task.path) > 1) buttons.push(queueButton("先装这个", () => queueAction("/api/queue/first", { path: task.path })));
    buttons.push(queueButton("移出", remove));
    return buttons;
  }
  const buttons = [];
  if (settledState(task) !== "installed" && task.retryable !== false) {
    buttons.push(queueButton(task.status === "interrupted" ? "继续" : "重试", () => queueAction("/api/install", { path: task.path })));
  }
  if (task.status === "interrupted") {
    const discard = queueButton("删除", async () => {
      discard.disabled = true;
      const error = await discardInstall(task);
      discard.disabled = false;
      if (error && error !== "cancelled") window.alert(error);
    }, "danger");
    buttons.push(discard);
  }
  buttons.push(queueButton("移出", remove));
  return buttons;
}

function paintQueueList() {
  const seen = new Set();
  tasks.forEach((task, index) => {
    seen.add(task.path);
    let row = queueRows.get(task.path);
    if (!row) {
      const item = el("li", "queue-item");
      const open = el("button", "queue-open");
      open.type = "button";
      const artSlot = el("span", "queue-art");
      const name = el("strong");
      const status = el("span");
      const text = el("span", "queue-text");
      text.append(name, status);
      open.append(artSlot, text);
      const actions = el("div", "queue-actions");
      const meter = el("span", "meter queue-meter");
      meter.append(el("span"));
      item.append(open, actions, meter);
      row = { item, open, artSlot, name, status, actions, meter, artKey: null, actionKey: null, task };
      open.addEventListener("click", () => openTask(row.task));
      queueRows.set(task.path, row);
    }
    row.task = task;
    row.item.dataset.state = task.state === "done" ? settledState(task) : task.state;
    row.name.textContent = displayName(task);
    row.status.textContent = jobText(task);
    const artKey = task.cover_url || "";
    if (row.artKey !== artKey) {
      row.artKey = artKey;
      row.artSlot.replaceChildren(buildArt(task));
    }
    row.meter.hidden = task.state !== "running";
    row.meter.style.setProperty("--p", taskFraction(task));
    const actionKey = [task.state, task.status, task.retryable, task.message === "正在取消…", queuePosition(task.path) > 1].join("|");
    if (row.actionKey !== actionKey) {
      row.actionKey = actionKey;
      row.actions.replaceChildren(...queueRowActions(task));
    }
    if (queueList.children[index] !== row.item) queueList.insertBefore(row.item, queueList.children[index] || null);
  });
  for (const [path, row] of queueRows) {
    if (seen.has(path)) continue;
    row.item.remove();
    queueRows.delete(path);
  }
}

dockAction.addEventListener("click", () => {
  const running = runningTask();
  if (running) queueAction("/api/queue/remove", { path: running.path });
  else queueAction("/api/queue/clear", {});
});

$("#dock-open").addEventListener("click", () => {
  if (tasks.length) openTask(dockSummary().head);
});

dockToggle.addEventListener("click", () => {
  dockOpen = !dockOpen;
  paintDock();
});

dockClear.addEventListener("click", () => queueAction("/api/queue/clear", {}));

/* 详情抽屉 */

const actionText = {
  install: "安装到主机",
  continue: "继续安装",
  update: "安装更新",
  installed: "已安装到主机",
  installing: "正在安装…",
  unsupported: "这种压缩包还不能安装",
  empty: "目录里没有压缩包",
  "need-settings": "先到设置里填写存放位置",
};

// 主机桌面注册状态。healthy 时状态写在主按钮上，不另起一行；其余给一句提示和要做的操作，不能注册时没有按钮。
function consoleSummary(info) {
  if (info.problem) return { healthy: false, text: info.problem, state: "warn", button: "" };
  if (info.files_ok === false) return { healthy: false, text: "游戏文件缺少执行权限，主机上启动不了", state: "warn", button: "修复" };
  if (info.registered && info.mounted) return { healthy: true };
  if (info.registered) return { healthy: false, text: "游戏目录没挂上，在主机桌面上启动不了", state: "warn", button: "修复" };
  if (info.error) return { healthy: false, text: `没能注册到主机桌面：${info.error}`, state: "warn", button: "重试" };
  return { healthy: false, text: "还没出现在主机桌面", state: "warn", button: "注册" };
}

function detailFacts(game) {
  const facts = [];
  const platform = platformText(game);
  if (game.title_id) facts.push(game.title_id);
  if (platform) facts.push(platform);
  if (regionText[game.region]) facts.push(regionText[game.region]);
  if (game.formats) facts.push(game.formats.toUpperCase());
  if (game.archive_bytes) facts.push(formatBytes(game.archive_bytes));
  return facts;
}

function langLabels(codes) {
  const names = codes.map((code) => langNames[code.toLowerCase().split("-")[0]] || code);
  return [...new Set(names)].join("、");
}

// 详情里的商店规格，没有的项不显示。
function storeSpecs(game) {
  const rows = [["发行商", game.publisher], ["类型", game.genres], ["发售日期", game.release_date]];
  if (game.store_platforms?.length) rows.push(["平台", game.store_platforms.join(" / ")]);
  if (game.edition) {
    const kind = game.classification && game.classification !== "FULL_GAME"
      ? `（${classificationText[game.classification] || game.classification}）` : "";
    rows.push(["版本", game.edition + kind]);
  }
  if (game.screen_langs?.length) rows.push(["字幕语言", langLabels(game.screen_langs)]);
  if (game.spoken_langs?.length) rows.push(["语音语言", langLabels(game.spoken_langs)]);
  if (game.players) rows.push(["本地玩家", game.players > 1 ? `1–${game.players} 人` : "单人"]);
  const online = [
    { REQUIRED: "需联网", OPTIONAL: "可联机" }[game.online_play] || "",
    game.ps_plus_required ? "联机需 PS Plus" : "",
    game.in_game_purchases ? "含内购" : "",
  ].filter(Boolean);
  if (online.length) rows.push(["联机与付费", online.join(" · ")]);
  return rows.filter(([, value]) => value);
}

function paintRating(box, game) {
  box.hidden = game.rating == null;
  if (box.hidden) return;
  const score = el("div", "rating-score");
  score.append(el("strong", "", Number(game.rating).toFixed(2)),
    el("span", "", `${(game.rating_count || 0).toLocaleString("zh-CN")} 人评分`));
  const parts = [score];
  if (Array.isArray(game.rating_dist) && game.rating_dist.length === 5) {
    const bars = el("div", "rating-bars");
    game.rating_dist.forEach((pct, index) => {
      const row = el("div");
      const track = el("i");
      const fill = el("b");
      fill.style.width = `${pct}%`;
      track.append(fill);
      row.append(el("span", "", `${5 - index}★`), track, el("span", "", `${pct}%`));
      bars.append(row);
    });
    parts.push(bars);
  }
  box.replaceChildren(...parts);
}

function nameSourceText(game) {
  if (game.name_source === "user") return "手动修改";
  if (game.name_source === "store") return storeLocaleText[game.store_locale] || "商店";
  if (game.name_source === "catalog") return "PlayStation-Titles 英文名";
  return "网盘目录名";
}

const layerRoleText = { main: "主游戏", patch: "补丁", extra: "附加" };

// 安装前挑选要装哪些压缩包组。主游戏必装；按列表从上到下叠加，后装的覆盖先装的同名文件。
function buildLayerPicker(onConfirm, onCancel) {
  const panel = el("div", "layer-picker");
  panel.hidden = true;
  const head = el("p", "picker-head", "选择要安装的内容");
  const hint = el("p", "picker-hint", "按从上到下的顺序叠加，后面的覆盖前面的同名文件。用不上的补丁可以不装。");
  const list = el("ul", "layer-list");
  const warn = el("p", "picker-warn");
  const total = el("p", "picker-total num");
  const actions = el("div", "picker-actions");
  const confirm = el("button", "btn primary", "开始安装");
  confirm.type = "button";
  const cancel = el("button", "btn quiet", "取消");
  cancel.type = "button";
  actions.append(confirm, cancel);
  panel.append(head, hint, list, warn, total, actions);

  let layers = [];
  let unchanged = false;
  const boxes = new Map();
  const refresh = () => {
    const chosen = layers.filter((layer) => boxes.get(layer.id).checked);
    const dropped = layers.filter((layer) => layer.installed && !boxes.get(layer.id).checked);
    const fresh = chosen.filter((layer) => !layer.installed);
    const bytes = (items) => items.reduce((sum, layer) => sum + layer.bytes, 0);
    warn.textContent = dropped.length
      ? `取消已装的「${dropped.map((layer) => layer.folder || layer.name).join("」「")}」后，要从头重装整个游戏，主游戏也会重新下载。`
      : "";
    warn.hidden = !warn.textContent;
    const installed = layers.some((layer) => layer.installed);
    total.textContent = installed && !dropped.length
      ? `这次新装 ${fresh.length} 项，共 ${formatBytes(bytes(fresh))}`
      : `共 ${chosen.length} 项，${formatBytes(bytes(chosen))}`;
    unchanged = installed && !dropped.length && !fresh.length &&
      layers.every((layer) => boxes.get(layer.id).checked !== Boolean(layer.skipped) || layer.role === "main");
    if (unchanged) confirm.textContent = "保持现状";
    else if (installed && !dropped.length) confirm.textContent = fresh.length ? "安装所选补丁" : "保存选择";
    else confirm.textContent = "开始安装";
  };

  confirm.addEventListener("click", () => {
    const skip = layers.filter((layer) => !boxes.get(layer.id).checked).map((layer) => layer.id);
    panel.hidden = true;
    if (unchanged) onCancel();
    else onConfirm(skip);
  });
  cancel.addEventListener("click", () => {
    panel.hidden = true;
    onCancel();
  });

  return {
    panel,
    open(items) {
      layers = items;
      boxes.clear();
      list.replaceChildren(...layers.map((layer) => {
        const item = el("li", "layer-item");
        const label = el("label");
        const box = el("input");
        box.type = "checkbox";
        box.checked = layer.role === "main" || !layer.skipped;
        box.disabled = layer.role === "main";
        box.addEventListener("change", refresh);
        boxes.set(layer.id, box);
        const text = el("span", "layer-text");
        const name = el("span", "layer-name", layer.folder || layer.name);
        const tags = el("span", "layer-tags");
        tags.append(el("span", `layer-tag ${layer.role}`, layerRoleText[layer.role] || ""));
        if (layer.installed) tags.append(el("span", "layer-tag installed", "已装"));
        const meta = [layer.folder ? layer.name : "", layer.parts > 1 ? `${layer.parts} 卷` : "", formatBytes(layer.bytes)];
        text.append(name, el("span", "layer-meta num", meta.filter(Boolean).join(" · ")));
        label.append(box, text, tags);
        item.append(label);
        return item;
      }));
      refresh();
      panel.hidden = false;
      confirm.focus();
    },
    close() {
      panel.hidden = true;
    },
  };
}

// 没装完的游戏上次装到哪里。总数按这次要装的压缩包算：已装好的层不会再解压。
function resumeProgress(game, task) {
  if (game.state !== "continue") return null;
  const record = game.resume || {};
  const finished = task && task.state === "done" && settledState(task) === "continue" ? task : {};
  const done = Math.max(record.bytes_done || 0, finished.bytes_done || 0);
  const files = Math.max(record.files_done || 0, finished.files_done || 0);
  if (!done && !files) return null;
  const chosen = (game.layers || []).filter((layer) => layer.role === "main" || !layer.skipped);
  const fresh = chosen.filter((layer) => !layer.installed);
  const planned = (fresh.length ? fresh : chosen).reduce((sum, layer) => sum + layer.bytes, 0) || game.archive_bytes || 0;
  const total = Math.max(planned, record.bytes_total || 0, finished.bytes_total || 0);
  return { done: Math.min(done, total || done), total, files };
}

// 详情里的删除确认，替换掉安装按钮那一行。有几步就要点几次；多步时最后一步的按钮先锁一会儿，
// 免得连点两下直接删掉。
function buildConfirm() {
  const panel = el("div", "confirm");
  panel.hidden = true;
  panel.setAttribute("role", "group");
  const top = el("div", "confirm-top");
  const head = el("p", "confirm-head");
  const steps = el("span", "confirm-steps");
  steps.setAttribute("role", "img");
  top.append(head, steps);
  const body = el("div", "confirm-body");
  const actions = el("div", "confirm-actions");
  const back = el("button", "btn quiet", "取消");
  back.type = "button";
  const go = el("button", "btn danger");
  go.type = "button";
  actions.append(back, go);
  panel.append(top, body, actions);
  let flow = null;
  let index = 0;
  let armTimer = 0;

  const showStep = () => {
    const step = flow.steps[index];
    const final = flow.steps.length > 1 && index === flow.steps.length - 1;
    head.textContent = step.head;
    body.replaceChildren(...step.body);
    steps.hidden = flow.steps.length < 2;
    steps.replaceChildren(...flow.steps.map((_, i) => el("i", i <= index ? "on" : "")));
    steps.setAttribute("aria-label", `第 ${index + 1} 步，共 ${flow.steps.length} 步`);
    panel.dataset.final = String(final);
    go.textContent = step.button;
    go.classList.remove("arming");
    window.clearTimeout(armTimer);
    go.disabled = final;
    if (final) {
      back.focus();
      void go.offsetWidth;
      go.classList.add("arming");
      armTimer = window.setTimeout(() => { go.disabled = false; }, 1200);
    }
  };
  const close = () => {
    window.clearTimeout(armTimer);
    flow = null;
    panel.hidden = true;
    go.classList.remove("arming");
    back.disabled = false;
  };
  go.addEventListener("click", async () => {
    if (index < flow.steps.length - 1) {
      index += 1;
      showStep();
      return;
    }
    const current = flow;
    go.disabled = true;
    back.disabled = true;
    go.classList.remove("arming");
    go.textContent = current.busy;
    await current.onConfirm();
    if (flow === current) close();
  });
  back.addEventListener("click", () => {
    const after = flow && flow.onCancel;
    close();
    if (after) after();
  });
  return {
    panel,
    get open() { return flow !== null; },
    get busy() { return back.disabled; },
    ask(options) {
      flow = options;
      index = 0;
      panel.hidden = false;
      back.disabled = false;
      showStep();
      back.focus();
    },
    cancel() { back.click(); },
    close,
  };
}

function openDetail(game, opener) {
  closeDetail();
  detailOverlay.hidden = false;
  document.body.classList.add("drawer-open");
  detailBody.replaceChildren();

  const hero = el("div", "hero");
  const backdrop = el("div", "hero-backdrop");
  const row = el("div", "hero-row");
  const artSlot = el("div");
  const heading = el("div");
  const title = el("h2");
  title.id = "detail-title";
  const facts = el("div", "facts num");
  heading.append(title, facts);
  row.append(artSlot, heading);
  const refreshMeta = el("button", "hero-refresh");
  refreshMeta.type = "button";
  refreshMeta.innerHTML = '<svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9"/><path d="M13.5 2.5v3h-3"/></svg>';
  hero.append(backdrop, row, refreshMeta);

  const install = el("div", "drawer-section install");
  const action = el("button", "btn primary");
  action.type = "button";
  const progress = el("div", "progress");
  const progressTop = el("div", "progress-top");
  const progressPct = el("span", "progress-pct num");
  const progressEta = el("span", "progress-eta");
  progressTop.append(progressPct, progressEta);
  const progressRow = el("div", "progress-row");
  const progressText = el("span");
  progressText.setAttribute("role", "status");
  const cancel = el("button", "link", "取消安装");
  cancel.type = "button";
  progressRow.append(progressText, cancel);
  const meter = el("span", "meter");
  meter.append(el("span"));
  progress.append(progressTop, meter, progressRow);
  const resume = el("div", "progress resume");
  const resumeMeter = el("span", "meter");
  resumeMeter.append(el("span"));
  const resumeText = el("span", "progress-row num");
  resume.append(resumeMeter, resumeText);
  const queueRow = el("div", "progress-row queue-row");
  const queueText = el("span");
  queueText.setAttribute("role", "status");
  const queueLinks = el("span", "queue-links");
  const first = el("button", "link", "先装这个");
  first.type = "button";
  const leave = el("button", "link", "移出队列");
  leave.type = "button";
  queueLinks.append(first, leave);
  queueRow.append(queueText, queueLinks);
  const status = el("p", "status-text");
  status.setAttribute("role", "status");
  // 不常用的操作（调整补丁、注册到主机、删除）收在主按钮旁的「⋯」菜单里，主按钮只管安装和显示状态。
  const menuItem = (text, className = "") => {
    const item = el("button", "more-item " + className, text);
    item.type = "button";
    item.setAttribute("role", "menuitem");
    return item;
  };
  const adjust = menuItem("调整要装的补丁");
  const registerItem = menuItem("注册到主机桌面");
  const discard = menuItem("删除已装的部分", "danger");
  const remove = menuItem("删除游戏", "danger");
  const more = el("button", "btn more");
  more.type = "button";
  more.setAttribute("aria-haspopup", "menu");
  more.setAttribute("aria-label", "更多操作");
  more.innerHTML = '<svg viewBox="0 0 16 16" width="18" height="18" aria-hidden="true" fill="currentColor"><circle cx="3" cy="8" r="1.5"/><circle cx="8" cy="8" r="1.5"/><circle cx="13" cy="8" r="1.5"/></svg>';
  const menu = el("div", "more-menu");
  menu.setAttribute("role", "menu");
  menu.append(adjust, registerItem, discard, remove);
  const moreWrap = el("div", "more-wrap");
  moreWrap.append(more, menu);
  let menuOpen = false;
  // 菜单开着时在别处按下鼠标就收起。不能靠焦点离开来判断：WebKit（PS5 自带浏览器）点按钮不会把焦点给按钮，
  // 点菜单项时焦点先丢了，菜单抢在点击生效前收起，点击就落空了。
  const closeOnOutside = (event) => {
    if (!moreWrap.contains(event.target)) setMenu(false);
  };
  const setMenu = (open) => {
    menuOpen = open;
    menu.hidden = !open;
    more.setAttribute("aria-expanded", String(open));
    if (open) {
      document.addEventListener("mousedown", closeOnOutside, true);
      menu.querySelector(".more-item:not([hidden])")?.focus();
    } else {
      document.removeEventListener("mousedown", closeOnOutside, true);
    }
  };
  setMenu(false);
  more.addEventListener("click", () => setMenu(!menuOpen));
  // 选了一项就收起来；用 Tab 把焦点移到菜单外的别的元素时也收起。
  menu.addEventListener("click", (event) => {
    if (event.target.closest(".more-item")) setMenu(false);
  });
  moreWrap.addEventListener("focusout", (event) => {
    if (menuOpen && event.relatedTarget && !moreWrap.contains(event.relatedTarget)) setMenu(false);
  });
  const actionRow = el("div", "action-row");
  actionRow.append(action, moreWrap);
  const confirm = buildConfirm();
  // 删除成功后的一句提示；队列里有这款游戏的结果时以队列为准。
  let notice = "";
  // picking：选择面板开着；waitingLayers：点了安装但压缩包列表还没读到，读到后再决定要不要先选。
  let picking = false;
  let waitingLayers = false;
  const picker = buildLayerPicker((skip) => {
    picking = false;
    status.hidden = true;
    notice = "";
    startInstall(game, skip);
    paint();
  }, () => {
    picking = false;
    paint();
    action.focus();
  });
  // 主机桌面注册有问题时才出现的一行提示，带上要做的那个操作；正常时状态写在主按钮上。
  const consoleRow = el("div", "console-note");
  const consoleText = el("span");
  consoleText.setAttribute("role", "status");
  const consoleAction = el("button", "btn small");
  consoleAction.type = "button";
  consoleRow.append(consoleText, consoleAction);
  let consoleBusy = false;

  // 评价：自己装过的版本能不能玩。评过的显示一条横幅；装好还没评的问一句；有问题时写一句说明。
  // 元素只建一次，paint 只改显隐和文字，队列每秒刷新时正在写的说明不会被冲掉。
  const reviewBox = el("div", "review");
  const reviewBanner = el("div", "review-banner");
  const reviewText = el("strong");
  const reviewMeta = el("span", "review-meta");
  reviewBanner.append(reviewText, reviewMeta);
  const reviewAsk = el("div", "review-ask");
  const reviewQuestion = el("span", "review-question");
  const reviewButtons = el("span", "review-buttons");
  const reviewGood = el("button", "btn small review-good", "能玩");
  const reviewBad = el("button", "btn small review-bad", "有问题");
  const reviewLater = el("button", "link", "以后再说");
  const reviewClear = el("button", "link", "清除评价");
  for (const button of [reviewGood, reviewBad, reviewLater, reviewClear]) button.type = "button";
  reviewButtons.append(reviewGood, reviewBad, reviewLater, reviewClear);
  reviewAsk.append(reviewQuestion, reviewButtons);
  const reviewNote = el("div", "review-note");
  const reviewChips = el("div", "review-chips");
  const noteInput = el("input");
  noteInput.maxLength = 100;
  noteInput.placeholder = "一句话说明问题（可不填）";
  for (const preset of ["进游戏黑屏", "闪退", "卡在加载", "要更高固件", "缺补丁"]) {
    const chip = el("button", "chip-toggle", preset);
    chip.type = "button";
    chip.addEventListener("click", () => {
      noteInput.value = preset;
      noteInput.focus();
    });
    reviewChips.append(chip);
  }
  const noteRow = el("div", "review-note-row");
  const noteSave = el("button", "btn small primary", "保存");
  const noteCancel = el("button", "link", "取消");
  noteSave.type = "button";
  noteCancel.type = "button";
  noteRow.append(noteInput, noteSave, noteCancel);
  reviewNote.append(el("span", "review-question", "哪里有问题？"), reviewChips, noteRow);
  reviewBox.append(reviewBanner, reviewAsk, reviewNote);
  // reviewMode：空的时候跟着状态显示；ask 是从菜单点了「修改评价」；note 正在写问题；later 这次不问了。
  let reviewMode = "";
  let reviewBusy = false;
  const reviewItem = menuItem("评价这个版本");
  menu.insertBefore(reviewItem, discard);
  // 评价时装的补丁组合和现在的一样吗（装好的按装着的比，没装的按选着要装的比）。
  // 评价没记组合、或者现在不知道装了哪几组（旧版装的没记）时不提示。
  const sameLayers = (review, installedNow) => {
    if (!review.layers || !review.layers.length) return true;
    const now = (game.layers || []).filter((layer) => (installedNow ? layer.installed : !layer.skipped))
      .map((layer) => layer.fingerprint);
    if (!now.length) return true;
    return review.layers.map((layer) => layer.fingerprint).sort().join("|") === now.sort().join("|");
  };
  const reviewWhen = (review) => {
    const date = serverClockDate(review.at * 1000);
    return `${date.getUTCMonth() + 1} 月 ${date.getUTCDate()} 日`;
  };
  const paintReview = (installedNow, busy) => {
    const review = game.review;
    const noting = !busy && reviewMode === "note";
    const asking = !busy && !noting && !picking && !confirm.open && Boolean(game.fingerprint) &&
      (reviewMode === "ask" || (reviewMode === "" && !review && installedNow));
    const shown = review || (!installedNow ? game.other_review : null);
    reviewNote.hidden = !noting;
    reviewAsk.hidden = !asking;
    reviewBanner.hidden = noting || asking || !shown;
    reviewBox.hidden = reviewNote.hidden && reviewAsk.hidden && reviewBanner.hidden;
    for (const button of [reviewGood, reviewBad, reviewClear, noteSave]) button.disabled = reviewBusy;
    if (asking) {
      reviewQuestion.textContent = review ? "这个版本能正常玩吗？" : "装好了，能正常玩吗？";
      reviewLater.textContent = review ? "取消" : "以后再说";
      reviewClear.hidden = !review;
      reviewGood.setAttribute("aria-pressed", String(Boolean(review && review.playable)));
      reviewBad.setAttribute("aria-pressed", String(Boolean(review && !review.playable)));
    }
    if (!reviewBanner.hidden) {
      const own = shown === review;
      const note = shown.note ? `：${shown.note}` : "";
      const who = shown.device ? `你在${shown.device}上` : "你";
      reviewBanner.dataset.state = own ? (shown.playable ? "good" : "bad") : "info";
      reviewText.textContent = own
        ? (shown.playable ? `${who}标记过这个版本能正常玩` : `${who}标记过这个版本有问题${note}`)
        : `这个游戏的另一个版本${who}标记过${shown.playable ? "能玩" : "有问题" + note}`;
      const meta = [shown.firmware ? `固件 ${shown.firmware}` : "固件未知", reviewWhen(shown)];
      if (own && !sameLayers(shown, installedNow)) meta.push("当时装的补丁和现在不同");
      reviewMeta.textContent = meta.join(" · ");
    }
  };
  const sendReview = async (endpoint, body) => {
    reviewBusy = true;
    paint();
    try {
      const data = await api(endpoint, { path: game.path, ...body });
      game.review = data.review;
      game.review_state = data.review ? (data.review.playable ? "good" : "bad") : "";
      reviewMode = "";
      noteInput.value = "";
      shareDetail(game);
    } catch (error) {
      if (detail && detail.game === game) detail.fail(error.message);
    }
    reviewBusy = false;
    if (detail && detail.game === game) paint();
  };
  reviewGood.addEventListener("click", () => sendReview("/api/review", { playable: true, note: "" }));
  reviewBad.addEventListener("click", () => {
    reviewMode = "note";
    noteInput.value = game.review && !game.review.playable ? game.review.note : "";
    paint();
    noteInput.focus();
  });
  noteSave.addEventListener("click", () => sendReview("/api/review", { playable: false, note: noteInput.value.trim() }));
  noteInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") noteSave.click();
  });
  noteCancel.addEventListener("click", () => {
    reviewMode = game.review ? "" : "later";
    paint();
  });
  reviewLater.addEventListener("click", () => {
    reviewMode = game.review ? "" : "later";
    paint();
  });
  reviewClear.addEventListener("click", () => sendReview("/api/review/remove", {}));
  reviewItem.addEventListener("click", () => {
    reviewMode = "ask";
    paint();
  });

  install.append(reviewBox, actionRow, confirm.panel, picker.panel, consoleRow, resume, progress, queueRow, status);

  const about = el("div", "drawer-section");
  const desc = el("p", "desc");
  const descMore = el("button", "desc-more");
  descMore.type = "button";
  let descOpen = false;
  descMore.addEventListener("click", () => {
    descOpen = !descOpen;
    paint();
  });
  const rating = el("div", "rating-box");
  const specs = el("dl", "specs");
  const metaNote = el("p", "meta-note");
  about.append(desc, descMore, rating, specs, metaNote);

  const naming = el("details", "naming");
  const summary = el("summary", "", "名称与资料");
  const namingBody = el("div", "naming-body");
  const origin = el("dl", "specs");
  // 网盘路径单独建一次：paint 会频繁重画，复制后的提示不能被冲掉。
  const pathText = el("span", "drive-path");
  const pathCopy = el("button", "path-copy", "复制");
  pathCopy.type = "button";
  const pathCell = el("dd", "path-cell");
  pathCell.append(pathText, pathCopy);
  let pathCopyTimer = 0;
  pathCopy.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(game.path);
      pathCopy.textContent = "已复制";
    } catch (error) {
      // 非本机的 http 页面拿不到剪贴板权限，退回到选中文字让人手动复制。
      window.getSelection().selectAllChildren(pathText);
      pathCopy.textContent = "按 Ctrl+C 复制";
    }
    clearTimeout(pathCopyTimer);
    pathCopyTimer = setTimeout(() => { pathCopy.textContent = "复制"; }, 2000);
  });
  const rename = buildRenameForm(game, () => {
    detail?.paint();
    shareMeta(game);
    scheduleMetaPoll(true);
  });
  namingBody.append(origin, rename.form);
  naming.append(summary, namingBody);

  detailBody.append(hero, install, about, naming);

  let coverKey = null;
  let archivesRead = false;
  // refreshing：正在向商店重新取资料；refreshResult：刚取完时图标短暂显示的结果（done / warn）。
  let refreshing = false;
  let refreshResult = "";
  const paint = () => {
    syncGameState(game);
    title.textContent = displayName(game);
    refreshMeta.hidden = !game.concept_id;
    refreshMeta.disabled = refreshing;
    refreshMeta.dataset.state = refreshing ? "busy" : refreshResult;
    const refreshLabel = refreshing ? "正在刷新商店资料…" : { done: "商店资料已更新", warn: "这次没有取到商店资料" }[refreshResult] || "刷新商店资料";
    refreshMeta.title = refreshLabel;
    refreshMeta.setAttribute("aria-label", refreshLabel);
    facts.replaceChildren(...detailFacts(game).map((text) => el("span", "", text)));
    if (coverKey !== (game.cover_url || "")) {
      coverKey = game.cover_url || "";
      artSlot.replaceChildren(buildArt(game, "hero-art"));
      backdrop.style.backgroundImage = coverKey ? `url("${coverKey}")` : "none";
    }
    paintArt(artSlot.firstChild, game);

    const task = taskFor(game.path);
    const running = task && task.state === "running";
    const queued = task && task.state === "queued";
    const position = queued ? queuePosition(game.path) : 0;
    const refused = task && task.state === "done" && task.retryable === false;
    let label = actionText[game.state] || "安装到主机";
    if (queued) label = "已加入安装队列";
    else if (refused) label = "不能直接安装";
    else if (queueBusy() && ["install", "continue", "update"].includes(game.state)) {
      label = { continue: "排队继续安装", update: "排队安装更新" }[game.state] || "加入安装队列";
    }
    if (waitingLayers) label = "正在读取压缩包…";
    const onConsole = game.console && ["installed", "update"].includes(game.state) ? consoleSummary(game.console) : null;
    if (game.state === "installed" && onConsole && onConsole.healthy) label = "已安装 · 已在主机桌面";
    action.textContent = label;
    action.classList.toggle("done", game.state === "installed");
    action.disabled = waitingLayers || running || queued || refused || ["installed", "unsupported", "empty"].includes(game.state) ||
      (game.state !== "need-settings" && archivesRead && !game.formats);
    if (picking && (running || queued)) {
      picking = false;
      picker.close();
    }
    if (confirm.open && !confirm.busy && (running || queued)) confirm.close();
    actionRow.hidden = picking || confirm.open;
    const busy = running || queued;
    discard.hidden = game.state !== "continue" || busy;
    remove.hidden = !["installed", "update"].includes(game.state) || busy;
    adjust.hidden = game.state !== "installed" || busy || (game.layers || []).length < 2;
    registerItem.hidden = !onConsole || Boolean(game.console.problem) || busy || consoleBusy;
    registerItem.textContent = onConsole && onConsole.healthy ? "重新注册到主机桌面" : "注册到主机桌面";
    const installedNow = ["installed", "update"].includes(game.state);
    reviewItem.hidden = !game.fingerprint || busy || (!game.review && !installedNow);
    reviewItem.textContent = game.review ? "修改评价" : "评价这个版本";
    paintReview(installedNow, busy);
    moreWrap.hidden = [adjust, registerItem, reviewItem, discard, remove].every((item) => item.hidden);
    if (moreWrap.hidden && menuOpen) setMenu(false);
    consoleRow.hidden = !onConsole || (onConsole.healthy && !consoleBusy) || picking || confirm.open || busy;
    if (!consoleRow.hidden) {
      consoleRow.dataset.state = consoleBusy ? "info" : onConsole.state;
      consoleText.textContent = consoleBusy ? "正在注册到主机桌面…" : onConsole.text;
      consoleAction.textContent = onConsole.button || "";
      consoleAction.hidden = consoleBusy || !onConsole.button;
    }
    const last = picking ? null : resumeProgress(game, task);
    resume.hidden = !last;
    if (last) {
      resumeMeter.style.setProperty("--p", last.total ? last.done / last.total : 0);
      resumeText.textContent = last.total
        ? `上次装到 ${formatBytes(last.done)} / ${formatBytes(last.total)}（${Math.floor((last.done / last.total) * 100)}%）· ${last.files} 个文件`
        : `上次已装好 ${last.files} 个文件`;
    }
    progress.hidden = !running;
    if (running) {
      progressText.textContent = jobText(task, true);
      meter.style.setProperty("--p", taskFraction(task));
      cancel.disabled = task.message === "正在取消…";
      const pace = taskPace(task);
      progressTop.hidden = !taskStarted(task);
      progressPct.textContent = Math.round(taskFraction(task) * 100) + "%";
      progressEta.textContent = pace && pace.left !== null ? `预计 ${finishClock(pace.left)} 装完` : "";
      progressEta.title = pace ? `从开始解压算的平均速度 ${pace.speed}` : "";
    }
    queueRow.hidden = !queued;
    if (queued) {
      queueText.textContent = position === 1 ? "排在第 1 个，当前这个装完就开始" : `排在第 ${position} 个`;
      first.hidden = position <= 1;
    }
    const finished = task && task.state === "done";
    if (running || queued) notice = "";
    status.hidden = !finished && !notice;
    status.className = "status-text" + (finished && settledState(task) !== "installed" ? " warn" : "");
    if (finished) status.textContent = jobText(task);
    else if (notice) status.textContent = notice;

    desc.textContent = game.description || "";
    desc.hidden = !game.description;
    const longDesc = (game.description || "").length > 160;
    desc.classList.toggle("clamped", longDesc && !descOpen);
    descMore.hidden = !longDesc;
    descMore.textContent = descOpen ? "收起" : "展开全部";
    paintRating(rating, game);
    specs.replaceChildren(...storeSpecs(game).flatMap(([key, value]) => [el("dt", "", key), el("dd", "", value)]));
    specs.hidden = specs.childElementCount === 0;
    metaNote.textContent = game.catalog_matched === false && !metaStateNote[game.meta_state]
      ? "资料库里没有这款游戏，仍然可以按目录名安装。"
      : metaStateNote[game.meta_state] || "";
    metaNote.hidden = !metaNote.textContent;
    about.hidden = desc.hidden && rating.hidden && specs.hidden && metaNote.hidden;

    const originRows = [["名称来源", nameSourceText(game)]];
    const english = (game.catalog_name || "").trim();
    if (english && english !== displayName(game)) originRows.push(["英文名", english]);
    const originNodes = originRows.flatMap(([key, value]) => [el("dt", "", key), el("dd", "", value)]);
    if (game.path) {
      pathText.textContent = game.path;
      originNodes.push(el("dt", "", "网盘路径"), pathCell);
    }
    if (game.concept_id) originNodes.push(el("dt", "", "Concept ID"), el("dd", "", String(game.concept_id)));
    origin.replaceChildren(...originNodes);
  };

  // 有多组压缩包时，新装和装更新前先让用户选要装哪些；继续安装沿用上次的选择。
  const openPicker = () => {
    picking = true;
    status.hidden = true;
    paint();
    picker.open(game.layers);
  };
  const beginInstall = () => {
    if (["install", "update"].includes(game.state) && (game.layers || []).length > 1) {
      openPicker();
      return;
    }
    status.hidden = true;
    notice = "";
    startInstall(game);
  };
  action.addEventListener("click", () => {
    if (game.state === "need-settings") {
      show("settings", "target");
      return;
    }
    if (!archivesRead && ["install", "update"].includes(game.state)) {
      waitingLayers = true;
      paint();
      return;
    }
    beginInstall();
  });
  adjust.addEventListener("click", openPicker);
  const registerConsole = async () => {
    consoleBusy = true;
    paint();
    try {
      game.console = (await api("/api/console/register", { path: game.path })).console;
      notice = "已注册到主机桌面。";
    } catch (error) {
      game.console = { ...game.console, error: error.message };
    }
    consoleBusy = false;
    if (detail && detail.game === game) paint();
  };
  consoleAction.addEventListener("click", registerConsole);
  registerItem.addEventListener("click", registerConsole);

  // 立即向商店重新取中文名、封面和简介：服务端把这款插到最前面，这里每秒问一次，取完就刷新详情和封面。
  refreshMeta.addEventListener("click", async () => {
    if (refreshing) return;
    refreshing = true;
    refreshResult = "";
    paint();
    const current = () => detail && detail.game === game;
    try {
      Object.assign(game, await api("/api/meta/refresh", { name: game.name }));
      for (let round = 0; round < 45 && game.meta_state === "pending" && current(); round += 1) {
        await new Promise((resolve) => window.setTimeout(resolve, 1000));
        const item = ((await api("/api/meta", { names: [game.name], detail: true })).items || [])[0];
        if (item && item.name === game.name) Object.assign(game, item);
      }
      refreshResult = game.meta_state === "ready" ? "done" : "warn";
    } catch (error) {
      refreshResult = "warn";
      if (current()) detail.fail(error.message);
    }
    refreshing = false;
    shareMeta(game);
    if (!current()) return;
    paint();
    rename.sync();
    window.setTimeout(() => {
      if (refreshing) return;
      refreshResult = "";
      if (current()) paint();
    }, 2000);
  });
  cancel.addEventListener("click", () => queueAction("/api/queue/remove", { path: game.path }));
  const removeWith = (endpoint, done, focusAfter) => async () => {
    const error = await removeInstall(game, endpoint);
    if (!detail || detail.game !== game) return;
    confirm.close();
    notice = error ? "" : done;
    paint();
    if (error) detail.fail(error);
    else reload();
    (error ? focusAfter : action).focus();
  };
  const askRemove = (steps, endpoint, done, focusAfter) => {
    status.hidden = true;
    confirm.ask({
      steps,
      busy: "正在删除…",
      onConfirm: removeWith(endpoint, done, focusAfter),
      onCancel: () => {
        paint();
        focusAfter.focus();
      },
    });
    paint();
  };
  discard.addEventListener("click", () => {
    askRemove([{
      head: "删除已经装了一部分的文件？",
      body: [el("p", "", "删除后再装要从头开始。")],
      button: "删除",
    }], "/api/discard", "已删除没装完的文件。", more);
  });
  remove.addEventListener("click", () => {
    const where = (game.install_dir || "").trim();
    const first = [el("p", "", "会删掉存放位置里这个游戏的整个文件夹：")];
    if (where) first.push(el("code", "confirm-path", where));
    if (game.console && game.console.registered) first.push(el("p", "", "主机桌面上这个游戏的图标也会一起移除。"));
    const size = game.archive_bytes ? `（压缩包 ${formatBytes(game.archive_bytes)}）` : "";
    askRemove([
      { head: `删除「${displayName(game)}」？`, body: first, button: "删除" },
      {
        head: "再确认一次",
        body: [el("p", "", `删除后不能恢复，想再玩要从网盘重新下载安装${size}。`)],
        button: "确认删除",
      },
    ], "/api/uninstall", "已从存放位置删除。", more);
  });
  first.addEventListener("click", () => queueAction("/api/queue/first", { path: game.path }));
  leave.addEventListener("click", () => queueAction("/api/queue/remove", { path: game.path }));

  // 简介等资料和上次读到的压缩包、安装情况都在本机缓存里，马上就有，先显示出来；
  // 实时列网盘目录、连主机要慢一些，读到了再换上。实时的先到了就不用缓存的。
  const loadCached = () => api("/api/game", { path: game.path, cached: true }).then((data) => {
    if (!detail || detail.game !== game || archivesRead) return;
    Object.assign(game, data);
    paint();
    rename.sync();
    if (data.source === "cache") shareDetail(game);
  }).catch(() => {
    // 实时的请求也会带回资料
  });

  const reload = () => api("/api/game", { path: game.path }).then((data) => {
    if (!detail || detail.game !== game) return;
    Object.assign(game, data);
    archivesRead = true;
    const pending = waitingLayers;
    waitingLayers = false;
    paint();
    rename.sync();
    shareDetail(game);
    if (pending && !action.disabled) beginInstall();
  }).catch((error) => {
    waitingLayers = false;
    if (detail && detail.game === game) {
      paint();
      detail.fail(error.message);
    }
  });

  detail = {
    game,
    opener,
    paint,
    reload,
    fail(message) {
      status.hidden = false;
      status.className = "status-text warn";
      status.textContent = message;
    },
    // Esc 先收起「⋯」菜单、关掉删除确认，再关详情。
    escape() {
      if (menuOpen) {
        setMenu(false);
        more.focus();
        return true;
      }
      if (!confirm.open || confirm.busy) return confirm.busy;
      confirm.cancel();
      return true;
    },
  };
  paint();
  loadCached();
  reload();
  $("#detail-close").focus();
}

function closeDetail() {
  if (detailOverlay.hidden) return;
  detailOverlay.hidden = true;
  document.body.classList.remove("drawer-open");
  const opener = detail && detail.opener;
  detail = null;
  if (opener && opener.isConnected) opener.focus();
}

function buildRenameForm(game, onSaved) {
  const formEl = el("form", "rename");
  const input = el("input");
  input.maxLength = 80;
  input.spellcheck = false;
  input.placeholder = "自己起个名字";
  input.setAttribute("aria-label", "自定义名称");
  const save = el("button", "btn primary small", "改名");
  save.type = "submit";
  const actions = el("div", "rename-actions");
  const reset = el("button", "btn quiet small", "恢复自动名称");
  reset.type = "button";
  actions.append(reset);
  const note = el("p", "note");
  note.setAttribute("role", "status");
  const line = el("div", "rename-line");
  line.append(input, save);
  formEl.append(line, actions, note);

  const sync = () => {
    input.value = game.name_source === "user" ? game.display_name : "";
    reset.hidden = game.name_source !== "user";
    actions.hidden = reset.hidden;
  };
  const send = async (path, body, done) => {
    note.textContent = "";
    note.classList.remove("error");
    try {
      Object.assign(game, await api(path, body));
      sync();
      onSaved();
      note.textContent = done;
    } catch (error) {
      note.classList.add("error");
      note.textContent = error.message;
    }
  };
  formEl.addEventListener("submit", (event) => {
    event.preventDefault();
    const value = input.value.trim();
    if (value) send("/api/name", { name: game.name, display_name: value }, "已改名。");
  });
  reset.addEventListener("click", () => send("/api/name", { name: game.name, display_name: "" }, "已恢复自动名称。"));
  sync();
  return { form: formEl, sync };
}

$("#detail-backdrop").addEventListener("click", closeDetail);
$("#detail-close").addEventListener("click", closeDetail);
document.addEventListener("keydown", (event) => {
  if (event.key !== "Escape" || detailOverlay.hidden) return;
  if (detail && detail.escape()) return;
  closeDetail();
});

/* 登录 */

function loginFlowActive() {
  return !qrWrap.hidden || loginTimer !== 0;
}

function setLoggedIn(on) {
  loggedIn = on;
  loginBox.hidden = on && !loginFlowActive();
  library.hidden = !loginBox.hidden;
  loginButton.hidden = on;
}

function stopLoginPoll() {
  window.clearTimeout(loginTimer);
  loginTimer = 0;
}

function finishLogin() {
  stopLoginPoll();
  qrWrap.hidden = true;
  userCode.textContent = "";
  loginText.textContent = "已经登录，正在读取游戏…";
  loadGames();
}

// 一次轮询会在服务端挂到半分钟，换了二维码后，旧二维码那次轮询的结果直接丢掉。
let loginRun = 0;

async function pollLogin() {
  loginTimer = 0;
  const run = loginRun;
  try {
    const poll = await api("/api/login/poll", {});
    if (run !== loginRun) return;
    if (poll.done) {
      finishLogin();
      return;
    }
    if (poll.expired || poll.error) {
      stopLoginPoll();
      qrWrap.hidden = true;
      loginText.textContent = poll.error || "二维码过期了，再显示一次。";
      return;
    }
    if (poll.scanned) loginText.textContent = "已扫码，在手机上点确认登录后，这里会自动进入游戏库。";
    loginTimer = window.setTimeout(pollLogin, Math.max(1, poll.retry_after || 1) * 1000);
  } catch (error) {
    if (run !== loginRun) return;
    stopLoginPoll();
    loginText.textContent = error.message;
  }
}

async function startLogin() {
  stopLoginPoll();
  loginRun += 1;
  loginButton.disabled = true;
  loginText.textContent = "正在向百度获取二维码…";
  let challenge;
  try {
    challenge = await api("/api/login/start", {});
  } finally {
    loginButton.disabled = false;
  }
  if (challenge.already) {
    qrWrap.hidden = true;
    loginText.textContent = "已经登录，正在读取游戏…";
    await loadGames();
    return;
  }
  if (challenge.qrcode_url) {
    qr.src = challenge.qrcode_url;
    qr.alt = "百度登录二维码";
    qrWrap.hidden = false;
  }
  userCode.textContent = challenge.user_code ? "用户码 " + challenge.user_code : "";
  loginText.textContent = "用百度网盘 App 扫码登录。";
  loginButton.textContent = "换一张二维码";
  loginTimer = window.setTimeout(pollLogin, Math.max(1, challenge.interval || 1) * 1000);
}

loginButton.addEventListener("click", () => {
  startLogin().catch((error) => { loginText.textContent = error.message; });
});

/* 设置 */

function formatTime(unixSeconds) {
  if (!unixSeconds || unixSeconds < 0) return "";
  const date = serverClockDate(unixSeconds * 1000);
  if (Number.isNaN(date.getTime())) return "";
  return `${date.getUTCFullYear()}/${date.getUTCMonth() + 1}/${date.getUTCDate()} ` +
    `${pad2(date.getUTCHours())}:${pad2(date.getUTCMinutes())}:${pad2(date.getUTCSeconds())}`;
}

function setTabStatus(name, text, state = "") {
  $(`#status-${name}`).textContent = text;
  $(`#dot-${name}`).dataset.state = state;
}

function renderCatalogStatus(info) {
  const sync = info && info.sync ? info.sync : {};
  if (sync.phase === "updating") {
    catalogStatus.replaceChildren(el("p", "catalog-meta", "正在后台同步…"));
    setTabStatus("catalog", "正在同步…");
    return;
  }
  if (!info || !info.loaded) {
    catalogStatus.replaceChildren(el("p", "catalog-meta", "还没有资料库，后台正在第一次同步。在这之前游戏按目录名显示。"));
    setTabStatus("catalog", "还没有资料库", "warn");
    return;
  }
  const count = info.count.toLocaleString("zh-CN");
  const figure = el("p", "catalog-figure");
  figure.append(el("span", "num", count), "款游戏");
  const parts = [figure];
  const updated = info.generated_at ? info.generated_at.replace("T", " ").replace("Z", " UTC")
    : formatTime(info.file_mtime) || formatTime(sync.last_success);
  const meta = [];
  if (updated) meta.push(`上次更新 ${updated}。`);
  if (info.stale) meta.push(`已超过 ${sync.stale_days || 7} 天，会自动重新同步。`);
  if (meta.length) parts.push(el("p", "catalog-meta", meta.join("")));
  if (sync.last_error) parts.push(el("p", "catalog-meta error", `最近一次同步失败：${sync.last_error}`));
  catalogStatus.replaceChildren(...parts);
  setTabStatus("catalog", `已收录 ${count} 款`, sync.last_error ? "warn" : "ok");
}

// 摘要只反映已保存的设置，所以只在读取和保存成功后刷新。
function summarizeSettings() {
  const library = form.elements.library_path.value.trim();
  const keysFilled = [...$("#drive-fields").querySelectorAll("input")].every((input) => input.value.trim());
  setTabStatus("drive", library || "还没填游戏库目录", library && keysFilled ? "ok" : "warn");

  const mode = form.querySelector('input[name="target"]:checked')?.value;
  const host = form.elements.ftp_host.value.trim();
  const dir = form.elements.local_dir.value.trim();
  if (mode === "ftp") {
    setTabStatus("target", host ? `FTP ${host}:${form.elements.ftp_port.value.trim() || "2121"}` : "还没填主机地址",
      host ? "ok" : "warn");
  } else if (mode === "local") {
    setTabStatus("target", dir || "还没填文件夹", dir ? "ok" : "warn");
  } else {
    setTabStatus("target", "还没选保存方式", "warn");
  }

  const dictionary = chosenDictionary();
  setTabStatus("unpack", dictionary ? `字典上限 ${dictionaryText(dictionary)}` : "字典不限制", dictionary ? "ok" : "warn");

  const verbose = form.elements.verbose_log.checked;
  setTabStatus("log", verbose ? "详细日志开着" : "只记要紧的", verbose ? "warn" : "ok");
}

let catalogTimer = 0;

async function loadCatalogStatus() {
  const info = await api("/api/catalog");
  renderCatalogStatus(info);
  window.clearInterval(catalogTimer);
  catalogTimer = 0;
  if (info && info.sync && info.sync.phase === "updating") {
    catalogTimer = window.setInterval(() => { loadCatalogStatus().catch(() => {}); }, 5000);
  }
}

// 每个插件的参数先记在这里，切换插件再切回来时还在。
let driveDrafts = new Map();
let currentDrive = "";

function rememberDrive() {
  const draft = driveDrafts.get(currentDrive);
  if (!draft) return;
  draft.library_path = form.elements.library_path.value;
  for (const field of draft.fields) field.value = form.elements["drive:" + field.key].value;
}

function renderDrive(id) {
  rememberDrive();
  currentDrive = id;
  const draft = driveDrafts.get(id);
  const box = $("#drive-fields");
  box.replaceChildren();
  box.hidden = draft.fields.length === 0;
  for (const field of draft.fields) {
    const label = el("label", "field", field.label);
    const input = el("input");
    input.name = "drive:" + field.key;
    input.type = field.secret ? "password" : "text";
    input.autocomplete = "off";
    input.spellcheck = false;
    input.value = field.value || "";
    label.append(input);
    if (field.hint) label.append(el("span", "hint", field.hint));
    box.append(label);
  }
  form.elements.library_path.value = draft.library_path || "";
  for (const radio of form.querySelectorAll('input[name="drive"]')) radio.checked = radio.value === id;
}

function renderDriveChoices(drives) {
  const box = $("#drive-choices");
  box.replaceChildren();
  for (const drive of drives) {
    const card = el("label", "choice-card");
    const radio = el("input");
    radio.type = "radio";
    radio.name = "drive";
    radio.value = drive.id;
    radio.addEventListener("change", () => renderDrive(drive.id));
    card.append(radio, el("strong", "", drive.name));
    if (drive.intro) card.append(el("span", "", drive.intro));
    box.append(card);
  }
}

/* 解压：字典上限，0 是不限制 */

const dictionaryPresets = [128, 256, 512, 1024, 2048, 4096, 8192, 0];
let defaultDictionary = 0;

function dictionaryText(mb) {
  if (!mb) return "不限制";
  return mb >= 1024 && mb % 1024 === 0 ? `${mb / 1024} GB` : `${mb} MB`;
}

function chosenDictionary() {
  const checked = form.querySelector('input[name="max_dictionary_mb"]:checked');
  return checked ? Number(checked.value) : defaultDictionary;
}

function paintDictionaryNote() {
  const mb = chosenDictionary();
  const text = mb
    ? `字典超过 ${dictionaryText(mb)} 的 RAR / 7z 不装。WinRAR 默认字典 32 MB，7-Zip 最高压缩 64 MB，大多数游戏包都在这以内。`
    : "再大的字典也照装。主机给不了那么多内存时会停下报「内存不够」，不自动重试。";
  $("#dict-note").textContent = `${text}带圆点的 ${dictionaryText(defaultDictionary)} 是这台设备的默认值。`;
}

function renderDictionary(value, fallback) {
  defaultDictionary = fallback;
  const values = [...dictionaryPresets];
  if (!values.includes(value)) values.splice(values.findIndex((mb) => !mb || mb > value), 0, value);
  const box = $("#dict-scale");
  box.replaceChildren(...values.map((mb) => {
    const option = el("label", "scale-step");
    const radio = el("input");
    radio.type = "radio";
    radio.name = "max_dictionary_mb";
    radio.value = String(mb);
    radio.checked = mb === value;
    radio.addEventListener("change", paintDictionaryNote);
    option.append(radio, el("span", "num", dictionaryText(mb)));
    if (mb === fallback) {
      option.dataset.default = "";
      option.title = "这台设备的默认值";
    }
    return option;
  }));
  paintDictionaryNote();
}

function renderTarget(mode) {
  for (const radio of form.querySelectorAll('input[name="target"]')) radio.checked = radio.value === mode;
  for (const group of form.querySelectorAll("[data-target]")) group.hidden = group.dataset.target !== mode;
}

async function loadSettings() {
  const settings = await api("/api/settings");
  driveDrafts = new Map(settings.drives.map((drive) => [drive.id, {
    library_path: drive.library_path,
    fields: drive.fields.map((field) => ({ ...field })),
  }]));
  currentDrive = "";
  renderDriveChoices(settings.drives);
  renderDrive(settings.drive);

  const targets = settings.targets || [];
  savedTarget = settings.target;
  runsOnConsole = !targets.includes("ftp");
  $("#target-choice").hidden = targets.length < 2;
  for (const radio of form.querySelectorAll('input[name="target"]')) {
    radio.closest(".choice-card").hidden = !targets.includes(radio.value);
  }
  form.elements.local_dir.placeholder = targets.includes("ftp") ? "D:\\PS5 游戏" : "/data/homebrew";
  for (const name of ["local_dir", "ftp_host", "ftp_port", "ftp_dir"]) form.elements[name].value = settings[name] || "";
  renderTarget(settings.target);
  renderDictionary(settings.max_dictionary_mb ?? settings.default_dictionary_mb, settings.default_dictionary_mb);
  form.elements.verbose_log.checked = Boolean(settings.verbose_log);
  $("#log-path").textContent = settings.log_path || "";
  summarizeSettings();
  await Promise.all([loadAccess(), loadCatalogStatus(), pollSpeedtest()]);
}

/* 局域网访问 */

const accessForm = $("#access-form");
const accessNote = $("#access-note");

function renderAccessCode() {
  $("#access-code").hidden = !accessForm.elements.require_code.checked;
}

function renderAccess(access) {
  const lan = accessForm.elements.lan;
  lan.checked = access.lan;
  // 从别的设备关掉局域网访问会把自己关在外面，只许在运行盘游的电脑上关。
  lan.disabled = access.lan_required || (access.lan && !access.local);
  $("#access-lan-hint").textContent = access.lan_required
    ? "主机上只能从其他设备打开盘游，所以一直开着"
    : lan.disabled ? "只能在运行盘游的电脑上关闭" : "同一网络里的设备可以打开盘游";
  accessForm.elements.require_code.checked = access.require_code;
  accessForm.elements.code.value = access.code || "";
  $("#access-url").hidden = !access.url;
  $("#access-url-text").textContent = access.url || "";
  renderAccessCode();
  if (!access.lan) setTabStatus("access", "只在本机使用");
  else setTabStatus("access", access.require_code ? "已开启，需要口令" : "已开启", "ok");
}

$("#access-copy").addEventListener("click", async () => {
  const text = $("#access-url-text").textContent;
  accessNote.classList.remove("error");
  try {
    await navigator.clipboard.writeText(text);
    accessNote.textContent = "地址已复制。";
  } catch (error) {
    // 非本机的 http 页面拿不到剪贴板权限，退回到选中文字让人手动复制。
    window.getSelection().selectAllChildren($("#access-url-text"));
    accessNote.textContent = "已选中地址，按 Ctrl+C 复制。";
  }
});

async function loadAccess() {
  accessNote.textContent = "";
  renderAccess(await api("/api/access"));
}

accessForm.elements.require_code.addEventListener("change", renderAccessCode);

accessForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  accessNote.classList.remove("error");
  try {
    renderAccess(await api("/api/access", {
      lan: accessForm.elements.lan.checked,
      require_code: accessForm.elements.require_code.checked,
      code: accessForm.elements.code.value.trim(),
    }));
    accessNote.textContent = "已保存。";
  } catch (error) {
    accessNote.classList.add("error");
    accessNote.textContent = error.message;
  }
});

for (const radio of form.querySelectorAll('input[name="target"]')) {
  radio.addEventListener("change", () => renderTarget(radio.value));
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  rememberDrive();
  const draft = driveDrafts.get(currentDrive);
  const body = {
    drive: currentDrive,
    library_path: draft.library_path,
    values: Object.fromEntries(draft.fields.map((field) => [field.key, field.value])),
    target: form.querySelector('input[name="target"]:checked')?.value || "",
  };
  for (const name of ["local_dir", "ftp_host", "ftp_port", "ftp_dir"]) body[name] = form.elements[name].value;
  body.max_dictionary_mb = chosenDictionary();
  body.verbose_log = form.elements.verbose_log.checked;
  settingsNote.classList.remove("error");
  try {
    await api("/api/settings", body);
    savedTarget = body.target;
    settingsNote.textContent = "已保存。";
    summarizeSettings();
    if (loggedIn) loadGames();
  } catch (error) {
    settingsNote.classList.add("error");
    settingsNote.textContent = error.message;
  }
});

async function downloadLog() {
  let response = await fetch("/api/log");
  if (response.status === 401) {
    await unlock();
    response = await fetch("/api/log");
  }
  if (!response.ok) {
    const data = await response.json().catch(() => ({}));
    throw new Error(data.error || "日志没有下载下来");
  }
  const blob = await response.blob();
  const name = /filename="([^"]+)"/.exec(response.headers.get("Content-Disposition") || "")?.[1] || "panplay.log";
  const url = URL.createObjectURL(blob);
  const link = el("a");
  link.href = url;
  link.download = name;
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

$("#log-download").addEventListener("click", async () => {
  const button = $("#log-download");
  const note = $("#log-note");
  button.disabled = true;
  note.classList.remove("error");
  note.textContent = "";
  try {
    await downloadLog();
  } catch (error) {
    note.classList.add("error");
    note.textContent = error.message;
  } finally {
    button.disabled = false;
  }
});

/* 下载测速：测速在运行盘游的设备上跑，这里只轮询近况 */

const speedRun = $("#speedtest-run");
const speedNote = $("#speedtest-note");
let speedTimer = 0;
let speedState = null;
let savedTarget = "";
let runsOnConsole = false;

function mbps(rate) {
  const value = rate / 1048576;
  return value >= 100 ? value.toFixed(0) : value.toFixed(1);
}

// 拿来比较的那次安装，服务端在测速开始时记下。
function speedReference(s) {
  if (!s.install) return null;
  const task = tasks.find((item) => item.path === s.install.path);
  const name = task ? displayName(task) : s.install.path.slice(s.install.path.lastIndexOf("/") + 1);
  return { name, speed: s.install.speed, running: s.install.running };
}

function stepLinks(step) {
  return step.connected > 0 && step.connected < step.connections ? step.connected : step.connections;
}

function speedBusy(s) {
  return s.phase === "finding" || s.phase === "testing";
}

function bestStep(s) {
  return s.steps.reduce((best, step) => (!best || step.rate > best.rate ? step : best), null);
}

// 先说网盘是不是按连接限速，再拿安装速度比：安装进度按压缩包字节算，和下载速度是同一个口径。
function speedVerdict(s) {
  const best = bestStep(s);
  if (!best || !(best.rate > 0)) return "";
  const single = (s.steps.find((step) => step.connections === 1) || best).rate;
  const write = savedTarget === "ftp" ? "解压或经 FTP 写进主机" : "解压或写盘";
  const parts = [];
  if (s.steps.length > 1) {
    parts.push(best.rate >= single * 1.3
      ? `网盘按连接限速：一条 ${mbps(single)} MB/s，${stepLinks(best)} 条一起能到 ${mbps(best.rate)} MB/s。`
      : `多开连接没有更快，总速度上限约 ${mbps(best.rate)} MB/s，卡在${runsOnConsole ? "主机" : "这台电脑"}的网络或网盘给这个账号的速度上。`);
  }
  const ref = speedReference(s);
  if (!ref) {
    parts.push(`装游戏时明显慢于 ${mbps(single)} MB/s，就是慢在${write}；接近 ${mbps(best.rate)} MB/s，就是慢在下载。`);
    return parts.join("");
  }
  const install = `「${ref.name}」安装平均 ${mbps(ref.speed)} MB/s`;
  if (ref.running) {
    parts.push(single >= ref.speed * 1.5
      ? `${install}，测速时它还在装，另开的连接仍能跑到 ${mbps(single)} MB/s：下载跟得上，慢在${write}。`
      : `测速时「${ref.name}」还在安装，两边分带宽，测出来偏低；等装完或取消后再测一次更准。`);
  } else if (ref.speed >= best.rate * 0.75) {
    parts.push(`${install}，已经接近网速上限：慢在下载。`);
  } else if (ref.speed < single * 0.5) {
    parts.push(`${install}，单连接就能跑 ${mbps(single)} MB/s：慢在${write}，不在下载。`);
  } else if (best.rate >= single * 1.3 && ref.speed < single * 1.2) {
    parts.push(`${install}，和单连接差不多。盘游只在解压等数据时才多开连接，所以多半慢在${write}。`);
  } else {
    parts.push(`${install}，是网速上限的 ${Math.round((ref.speed / best.rate) * 100)}%：下载和${write}都可能在拖。`);
  }
  return parts.join("");
}

function speedLadder(s) {
  const busy = speedBusy(s);
  const best = bestStep(s);
  const live = s.measuring;
  const top = Math.max(1, ...s.steps.map((step) => step.rate), live ? live.rate : 0);
  return s.levels.map((level) => {
    const step = s.steps.find((item) => item.connections === level);
    const row = el("li", "speedtest-step");
    const meter = el("span", "meter");
    meter.append(el("span"));
    const value = el("b", "num");
    let rate = 0;
    if (step) {
      rate = step.rate;
      row.classList.add(step === best && s.steps.length > 1 ? "best" : "done");
      value.textContent = `${mbps(rate)} MB/s`;
    } else if (busy && live && live.connections === level) {
      rate = live.rate;
      row.classList.add("live");
      value.textContent = `${mbps(rate)} MB/s`;
    } else if (busy && s.connections === level) {
      row.classList.add("live");
      value.textContent = "连接中…";
    } else if (!busy) {
      value.textContent = "—";
    }
    meter.style.setProperty("--p", Math.min(1, rate / top));
    const counted = step || (busy && live && live.connections === level ? live : null);
    const short = counted && stepLinks(counted) < level;
    const label = el("span", "", short ? `${counted.connected}/${level} 条` : `${level} 条`);
    if (short) label.title = `这一档只连上 ${counted.connected} 条，其余一直没收到数据`;
    row.append(label, meter, value);
    return row;
  });
}

function paintSpeedtest(s) {
  speedState = s;
  const busy = speedBusy(s);
  speedRun.disabled = false;
  speedRun.textContent = busy ? "停止" : s.phase === "idle" ? "开始测速" : "再测一次";
  const best = bestStep(s);
  $("#speedtest-board").hidden = !(s.phase === "testing" || s.steps.length);
  if (busy) {
    $("#speedtest-speed").textContent = mbps(s.current);
    $("#speedtest-label").textContent = s.connections ? `${s.connections} 条连接同时读` : "";
  } else {
    $("#speedtest-speed").textContent = best ? mbps(best.rate) : "0";
    $("#speedtest-label").textContent = best ? `最快，${stepLinks(best)} 条连接` : "";
  }
  $("#speedtest-ladder").replaceChildren(...speedLadder(s));
  const meta = [s.file];
  if (s.bytes) meta.push(busy ? `已读 ${formatBytes(s.bytes)}` : `${s.seconds} 秒读了 ${formatBytes(s.bytes)}`);
  if (s.first_byte_ms) meta.push(`首字节 ${(s.first_byte_ms / 1000).toFixed(2)} 秒`);
  $("#speedtest-meta").textContent = meta.filter(Boolean).join(" · ");

  const ref = speedReference(s);
  const verdict = s.phase === "done" ? speedVerdict(s) : "";
  $("#speedtest-verdict").hidden = !verdict;
  $("#speedtest-verdict").textContent = verdict;

  speedNote.classList.remove("error", "warn");
  if (s.phase === "finding") {
    speedNote.textContent = "正在游戏库里找个大文件来测…";
  } else if (s.phase === "error") {
    speedNote.classList.add("error");
    speedNote.textContent = `测速没有完成：${s.error}`;
  } else if (busy && ref && ref.running) {
    speedNote.classList.add("warn");
    speedNote.textContent = "正在安装，测速和安装会分带宽。";
  } else if (s.phase === "done" && s.ended === "stopped") {
    speedNote.textContent = s.steps.length ? "已停止，上面是停下前量到的。" : "已停止。";
  } else {
    speedNote.textContent = "";
  }
}

async function pollSpeedtest() {
  window.clearTimeout(speedTimer);
  speedTimer = 0;
  let status;
  try {
    status = await api("/api/speedtest");
  } catch (error) {
    speedTimer = window.setTimeout(pollSpeedtest, 2000);
    return;
  }
  paintSpeedtest(status);
  if (speedBusy(status)) speedTimer = window.setTimeout(pollSpeedtest, 500);
}

speedRun.addEventListener("click", async () => {
  speedRun.disabled = true;
  try {
    if (speedState && speedBusy(speedState)) {
      paintSpeedtest(await api("/api/speedtest/stop", {}));
    } else {
      paintSpeedtest(await api("/api/speedtest/start", {}));
    }
    pollSpeedtest();
  } catch (error) {
    speedRun.disabled = false;
    speedNote.classList.remove("warn");
    speedNote.classList.add("error");
    speedNote.textContent = error.message;
    // 别的页面已经开始测了，就接着显示那一次。
    const status = await api("/api/speedtest").catch(() => null);
    if (status && speedBusy(status)) pollSpeedtest();
  }
});

catalogReload.addEventListener("click", async () => {
  catalogReload.disabled = true;
  catalogNote.classList.remove("error");
  catalogNote.textContent = "正在重新加载…";
  try {
    renderCatalogStatus(await api("/api/catalog/reload", {}));
    catalogNote.textContent = "已重新加载。";
    if (loggedIn) await loadGames();
  } catch (error) {
    catalogNote.classList.add("error");
    catalogNote.textContent = error.message;
  } finally {
    catalogReload.disabled = false;
  }
});

$("#quit").addEventListener("click", async () => {
  const button = $("#quit");
  const note = $("#quit-note");
  const ask = queueBusy()
    ? "退出盘游？正在进行的安装会停下，下次启动盘游时接着装。"
    : "退出盘游？退出后要重新启动盘游才能打开这个页面。";
  if (!window.confirm(ask)) return;
  button.disabled = true;
  note.textContent = "";
  try {
    await api("/api/quit", {});
  } catch (error) {
    note.textContent = error.message;
    button.disabled = false;
    return;
  }
  stopQueueWatch();
  window.clearTimeout(metaTimer);
  window.clearTimeout(loginTimer);
  closeDetail();
  dock.hidden = true;
  $("#quit-box").hidden = false;
});

/* 导航与筛选 */

const settingsTabs = [...document.querySelectorAll(".settings-tab")];

function showSettingsTab(name) {
  for (const tab of settingsTabs) {
    const selected = tab.dataset.panel === name;
    tab.setAttribute("aria-selected", String(selected));
    tab.tabIndex = selected ? 0 : -1;
    $(`#${tab.getAttribute("aria-controls")}`).hidden = !selected;
  }
  // 网盘和存放位置共用一个表单和保存按钮，两个都没选中时整个表单收起来。
  form.hidden = ![...form.querySelectorAll('[role="tabpanel"]')].some((panel) => !panel.hidden);
}

settingsTabs.forEach((tab, index) => {
  tab.addEventListener("click", () => showSettingsTab(tab.dataset.panel));
  tab.addEventListener("keydown", (event) => {
    const step = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1 }[event.key];
    let next;
    if (step) next = settingsTabs[(index + step + settingsTabs.length) % settingsTabs.length];
    else if (event.key === "Home") next = settingsTabs[0];
    else if (event.key === "End") next = settingsTabs[settingsTabs.length - 1];
    if (!next) return;
    event.preventDefault();
    showSettingsTab(next.dataset.panel);
    next.focus();
  });
});

function show(view, settingsTab) {
  closeDetail();
  shelfView.hidden = view !== "shelf";
  settingsView.hidden = view !== "settings";
  for (const [selector, name] of [["#show-shelf", "shelf"], ["#show-settings", "settings"]]) {
    if (name === view) $(selector).setAttribute("aria-current", "page");
    else $(selector).removeAttribute("aria-current");
  }
  if (settingsTab) showSettingsTab(settingsTab);
  if (view === "settings") {
    settingsNote.textContent = "";
    loadSettings().catch((error) => {
      settingsNote.classList.add("error");
      settingsNote.textContent = error.message;
    });
  }
  window.scrollTo(0, 0);
}

$("#show-shelf").addEventListener("click", () => show("shelf"));
$("#show-settings").addEventListener("click", () => show("settings"));

search.addEventListener("input", () => {
  if (shelfView.hidden) show("shelf");
  window.clearTimeout(searchTimer);
  searchTimer = window.setTimeout(() => {
    const next = search.value.trim().toLowerCase();
    if (next === query) return;
    query = next;
    reloadShelf();
  }, 250);
});

for (const button of filterButtons) {
  button.addEventListener("click", () => {
    if (filter === button.dataset.filter) return;
    filter = button.dataset.filter;
    for (const other of filterButtons) other.setAttribute("aria-pressed", String(other === button));
    reloadShelf();
  });
}

for (const button of refineButtons) {
  button.addEventListener("click", () => {
    const key = button.dataset.refine;
    refine[key] = !refine[key];
    button.setAttribute("aria-pressed", String(refine[key]));
    reloadShelf();
  });
}

sortSelect.addEventListener("change", () => {
  sortBy = sortSelect.value;
  reloadShelf();
});

/* 启动 */

async function boot() {
  loginButton.disabled = true;
  try {
    const session = await api("/api/session");
    if (!session.logged_in) {
      setLoggedIn(false);
      loginText.textContent = signedOutHint;
      return;
    }
    await refreshQueue();
    await loadGames();
  } catch (error) {
    setLoggedIn(false);
    loginText.textContent = error.message;
  } finally {
    loginButton.disabled = false;
  }
}

boot();
