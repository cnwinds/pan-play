#include "panplay/store_meta.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>

namespace panplay {
namespace {

int fail(const std::string& message) {
  std::cerr << "商店资料自检失败: " << message << "\n";
  return 1;
}

std::string env_script(const std::string& id, const std::string& json) {
  return "<script id=\"env:" + id + "\" type=\"application/json\">" + json + "</script>";
}

std::string sample_page(std::int64_t concept_id, const std::string& concept_name,
                        const std::string& image = "https://img.test/m.png") {
  const std::string key = "Concept:" + std::to_string(concept_id);
  return "<html><head><script id=\"wca-config\" type=\"application/json\">{\"env\":\"production\"}</script></head><body>" +
         env_script("a", "{\"args\":{},\"cache\":{\"" + key + "\":{\"id\":\"" + std::to_string(concept_id) +
                             "\",\"name\":\"" + concept_name +
                             "\",\"defaultProduct\":{\"__ref\":\"Product:HP0001-PPSA00001_00-X\"},\"media\":["
                             "{\"type\":\"IMAGE\",\"role\":\"SCREENSHOT\",\"url\":\"https://img.test/s.jpg\"},"
                             "{\"type\":\"IMAGE\",\"role\":\"MASTER\",\"url\":\"" + image + "\"}]}}}") +
         env_script("b", "{\"cache\":{\"" + key +
                             "\":{\"publisherName\":\"盘游工作室\",\"localizedGenres\":[{\"value\":\"动作\"},{\"value\":\"冒险\"}],"
                             "\"releaseDate\":{\"value\":\"2024-05-01T00:00:00Z\"},\"descriptions\":[{\"type\":\"LONG\","
                             "\"value\":\"第一行<br>第二行 &amp; 更多\"}]},\"Product:HP0001-PPSA00001_00-X\":{\"name\":"
                             "\"测试游戏 (简体中文, 英语)\"}}}") +
         "</body></html>";
}

std::string empty_page() { return env_script("x", "{\"cache\":{},\"translations\":{}}"); }

int check_names() {
  const std::pair<const char*, const char*> cases[] = {
      {"Ghost of Tsushima 导演剪辑版 (泰语, 日语, 韩语, 简体中文, 繁体中文, 英语)", "Ghost of Tsushima 导演剪辑版"},
      {"狂想樂園 (中韓文版)", "狂想樂園"},
      {"《失落之魂》", "失落之魂"},
      {"游戏（简体中文版）", "游戏"},
      {"Marvel's Spider-Man 2 (PS5)", "Marvel's Spider-Man 2 (PS5)"},
      {"  ASTRO BOT  ", "ASTRO BOT"},
  };
  for (const auto& [input, expected] : cases) {
    if (clean_store_name(input) != expected) return fail(std::string("名称清洗不对: ") + input + " -> " + clean_store_name(input));
  }
  return 0;
}

int check_parse() {
  const StoreMeta meta = parse_concept_page(sample_page(42, "《测试游戏》"), 42);
  if (!meta.found || meta.name != "测试游戏") return fail("没有取到概念名: " + meta.name);
  if (meta.cover_url != "https://img.test/m.png") return fail("封面应优先用 MASTER: " + meta.cover_url);
  if (meta.publisher != "盘游工作室" || meta.genres != "动作 / 冒险") return fail("发行商或类型不对");
  if (meta.release_date != "2024-05-01") return fail("发售日不对: " + meta.release_date);
  if (meta.description != "第一行\n第二行 & 更多") return fail("简介没有去掉标记: " + meta.description);

  const StoreMeta product_only = parse_concept_page(sample_page(42, ""), 42);
  if (product_only.name != "测试游戏") return fail("概念没有名称时应回退到默认商品名: " + product_only.name);
  if (parse_concept_page(empty_page(), 42).found) return fail("空页面不应算收录");
  if (parse_concept_page(sample_page(42, "x"), 43).found) return fail("别的概念不应算收录");
  return 0;
}

int check_cache() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("panplay-store-meta-test-" + std::to_string(std::rand()));
  std::filesystem::remove_all(dir);

  std::map<std::string, HttpResponse> routes;
  routes["https://store.playstation.com/zh-hans-hk/concept/1"] = {200, empty_page(), ""};
  routes["https://store.playstation.com/zh-hant-hk/concept/1"] = {200, sample_page(1, "測試遊戲"), ""};
  routes["https://img.test/m.png?w=440"] = {200, std::string("\xFF\xD8\xFF\xE0jpeg", 8), ""};
  routes["https://store.playstation.com/zh-hans-hk/concept/2"] = {404, "", ""};
  routes["https://store.playstation.com/zh-hant-hk/concept/2"] = {200, empty_page(), ""};
  routes["https://store.playstation.com/en-us/concept/2"] = {200, empty_page(), ""};
  routes["https://store.playstation.com/zh-hans-hk/concept/3"] = {403, "", ""};
  routes["https://store.playstation.com/zh-hans-hk/concept/4"] = {200, sample_page(4, "封面挂了", "https://img.test/down.png"), ""};
  routes["https://store.playstation.com/zh-hans-hk/concept/5"] = {200, sample_page(5, "处理中又刷新"), ""};
  int requests = 0;
  StoreMetaCache* active = nullptr;
  bool refreshed_mid_flight = false;
  const HttpFetch fake = [&](const HttpRequest& request) {
    ++requests;
    if (active != nullptr && !refreshed_mid_flight && request.url.find("/concept/5") != std::string::npos) {
      refreshed_mid_flight = true;
      active->refresh(5);
    }
    const auto it = routes.find(request.url);
    if (it == routes.end()) return HttpResponse{0, "", "unexpected " + request.url};
    return it->second;
  };

  int result = 0;
  {
    StoreMetaCache cache(dir, fake, 0);
    StoreMeta meta;
    if (cache.lookup(1, &meta) != StoreMetaState::pending) result = fail("首次查询应排队");
    cache.lookup(2, nullptr);
    cache.lookup(3, nullptr);
    while (cache.run_once()) {
    }
    if (!result && cache.lookup(1, &meta) != StoreMetaState::ready) result = fail("补齐后应就绪");
    if (!result && (meta.locale != "zh-hant-hk" || meta.name != "測試遊戲")) result = fail("应回退到港服繁体: " + meta.locale);
    if (!result && !meta.has_cover) result = fail("封面没有落到本机");
    if (!result && cache.lookup(2, nullptr) != StoreMetaState::missing) result = fail("三个地区都没有时应记为未收录");
    if (!result && cache.lookup(3, nullptr) != StoreMetaState::offline) result = fail("商店拒绝时应稍后重试");

    if (!result) {
      cache.lookup(4, nullptr);
      while (cache.run_once()) {
      }
      const int before = requests;
      if (cache.lookup(4, &meta) != StoreMetaState::ready || meta.has_cover) result = fail("封面下载失败时资料仍应就绪");
      else if (cache.run_once() || requests != before) result = fail("封面失败后不应立刻反复重试");
    }
    if (!result) {
      const int before = requests;
      cache.refresh(1);
      if (cache.lookup(1, nullptr) != StoreMetaState::pending) result = fail("重新获取时应显示补齐中");
      while (cache.run_once()) {
      }
      if (!result && requests - before != 3) result = fail("重新获取应重抓页面和封面，实际请求 " + std::to_string(requests - before));
    }
    if (!result) {
      active = &cache;
      cache.lookup(5, nullptr);
      if (!cache.run_once() || !refreshed_mid_flight) result = fail("没有处理概念 5");
      else if (cache.lookup(5, nullptr) != StoreMetaState::pending) result = fail("处理中途的重新获取被吞掉了");
      while (cache.run_once()) {
      }
      active = nullptr;
      if (!result && cache.lookup(5, nullptr) != StoreMetaState::ready) result = fail("概念 5 最终应就绪");
    }
  }
  if (!result) {
    const int before = requests;
    StoreMetaCache reopened(dir, fake, 0);
    StoreMeta meta;
    if (reopened.lookup(1, &meta) != StoreMetaState::ready || meta.name != "測試遊戲" || !meta.has_cover) {
      result = fail("重开后没有读到缓存");
    } else if (reopened.lookup(2, nullptr) != StoreMetaState::missing) {
      result = fail("未收录记录没有持久化");
    } else if (reopened.lookup(3, nullptr) != StoreMetaState::pending) {
      result = fail("网络失败不应写进缓存");
    } else if (requests != before) {
      result = fail("命中缓存时不应联网");
    }
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return result;
}

}  // namespace

int run_store_meta_self_test() {
  if (const int code = check_names()) return code;
  if (const int code = check_parse()) return code;
  return check_cache();
}

}  // namespace panplay
