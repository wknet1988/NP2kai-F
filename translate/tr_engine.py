#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NP2kai 内部翻译机能 —— 多后端免费翻译引擎（tr_engine.py）

调用协议（模拟器以此方式调用）：  engine <输入文件> <输出文件>
  * 输入文件：UTF-8，每行一条原文
  * 输出文件：UTF-8，每行一条译文（与输入行数、顺序一致）

支持的后端（免费、无需付费密钥即可用，或注册免费额度）：
  * google  —— Google 翻译网页接口（无需密钥，国内可能需换 host）
  * bing    —— 微软 Edge 翻译接口（无需密钥）
  * baidu   —— 百度翻译开放平台（需免费 APPID / KEY，注册地址见下）
  * youdao  —— 有道智云（需免费 APPKEY / APPSECRET，注册地址见下）

仅使用 Python 标准库，无需 pip 安装任何依赖。

=== 配置 ===
配置来源（优先级从高到低）：
  1) 环境变量
  2) 配置文件 tr_config.ini（与本脚本同目录，或 TR_CONFIG 指定路径）
  3) 内置默认值

配置项（[translate] 段，或大写环境变量）：
  backend       后端顺序，逗号分隔，例如 "google,bing,baidu,youdao"
                第一个可用的后端会优先尝试，某行失败时自动切换到下一个。
  src           源语言，默认 auto（自动识别）。PC-98 游戏为日语，可设 ja。
  dst           目标语言，默认 zh（简体中文）。
  sleep         每行之间的间隔秒数（用于遵守免费接口的频率限制），默认 0.15
  retries       单个后端单次翻译的最大重试次数，默认 3
  retry_delay   重试基础退避秒数，默认 0.5（实际等待 = delay * 2^次数）
  timeout       单次 HTTP 超时秒数，默认 10
  google_host   Google 接口主机，默认 translate.googleapis.com
                国内网络可尝试 translate.google.cn
  baidu_appid / baidu_key      百度翻译 APPID / 密钥（免费注册 https://fanyi-api.baidu.com/ ）
  youdao_appkey / youdao_appsecret   有道智云 APPKEY / APPSECRET（免费注册 https://ai.youdao.com/ ）

示例 tr_config.ini：
  [translate]
  backend = baidu,youdao,bing,google
  src = auto
  dst = zh
  baidu_appid = 你的APPID
  baidu_key   = 你的密钥

=== 调试 ===
  python tr_engine.py --test "こんにちは"
  python tr_engine.py --list
"""

import sys
import os
import re
import json
import time
import random
import hashlib
import urllib.request
import urllib.parse
import urllib.error
import configparser


# ----------------------------------------------------------------------------
# 配置读取
# ----------------------------------------------------------------------------

def _script_dir():
    return os.path.dirname(os.path.abspath(__file__))


def _load_config():
    """返回 dict：配置项名(小写) -> 字符串。环境变量优先于配置文件。"""
    cfg = {}

    # 1) 配置文件
    cfg_path = os.environ.get("TR_CONFIG")
    if not cfg_path:
        cfg_path = os.path.join(_script_dir(), "tr_config.ini")
    if os.path.exists(cfg_path):
        try:
            cp = configparser.ConfigParser()
            cp.read(cfg_path, encoding="utf-8")
            if cp.has_section("translate"):
                for k, v in cp.items("translate"):
                    cfg[k.lower()] = v.strip()
        except Exception as e:
            sys.stderr.write("[tr_engine] 读取配置失败 %s: %s\n" % (cfg_path, e))

    # 2) 环境变量（覆盖配置文件）
    env_map = {
        "backend": "TR_BACKEND",
        "src": "TR_SRC",
        "dst": "TR_DST",
        "sleep": "TR_SLEEP",
        "retries": "TR_RETRIES",
        "retry_delay": "TR_RETRY_DELAY",
        "timeout": "TR_TIMEOUT",
        "google_host": "TR_GOOGLE_HOST",
        "baidu_appid": "BAIDU_APPID",
        "baidu_key": "BAIDU_KEY",
        "youdao_appkey": "YOUDAO_APPKEY",
        "youdao_appsecret": "YOUDAO_APPSECRET",
    }
    for key, env in env_map.items():
        if env in os.environ and os.environ[env]:
            cfg[key] = os.environ[env].strip()

    return cfg


CFG = _load_config()


def _get(name, default):
    v = CFG.get(name)
    return v if v is not None else default


def _get_float(name, default):
    try:
        return float(_get(name, default))
    except ValueError:
        return default


def _get_int(name, default):
    try:
        return int(_get(name, default))
    except ValueError:
        return default


SRC = _get("src", "auto")
DST = _get("dst", "zh")
SLEEP = _get_float("sleep", 0.15)
RETRIES = _get_int("retries", 3)
RETRY_DELAY = _get_float("retry_delay", 0.5)
TIMEOUT = _get_float("timeout", 10.0)
GOOGLE_HOST = _get("google_host", "translate.googleapis.com")

BAIDU_APPID = _get("baidu_appid", "")
BAIDU_KEY = _get("baidu_key", "")
YOUDAO_APPKEY = _get("youdao_appkey", "")
YOUDAO_APPSECRET = _get("youdao_appsecret", "")

BACKEND_ORDER = [b.strip().lower() for b in _get("backend", "google,bing,baidu,youdao").split(",") if b.strip()]


# ----------------------------------------------------------------------------
# 语言代码映射（内部 auto/ja/zh -> 各后端自己的代码）
# ----------------------------------------------------------------------------

def _g_code(code):
    return {"zh": "zh-CN", "ja": "ja", "auto": "auto", "en": "en"}.get(code, code)


def _bing_code(code):
    # Edge 翻译：目标用 zh-Hans；源可省略(auto-detect)
    return {"zh": "zh-Hans", "ja": "ja", "auto": "auto", "en": "en"}.get(code, code)


def _baidu_code(code):
    # 百度：日语是 jp（不是 ja）
    return {"zh": "zh", "ja": "jp", "auto": "auto", "en": "en", "ko": "kor"}.get(code, code)


def _youdao_code(code):
    # 有道：简体中文是 zh-CHS
    return {"zh": "zh-CHS", "ja": "ja", "auto": "auto", "en": "en", "ko": "ko"}.get(code, code)


# ----------------------------------------------------------------------------
# HTTP 工具
# ----------------------------------------------------------------------------

_UA = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0 Safari/537.36"
    )
}


class TranslationError(Exception):
    pass


def _http_get(url, timeout=TIMEOUT, headers=None):
    h = dict(_UA)
    h.update(headers or {})
    req = urllib.request.Request(url, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        raise TranslationError("HTTP %s" % e.code)
    except Exception as e:
        raise TranslationError(str(e))


def _http_post(url, data_bytes, headers, timeout=TIMEOUT):
    h = dict(_UA)
    h.update(headers or {})
    req = urllib.request.Request(url, data=data_bytes, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        raise TranslationError("HTTP %s" % e.code)
    except Exception as e:
        raise TranslationError(str(e))


def _with_retry(fn):
    last = None
    for attempt in range(max(1, RETRIES)):
        try:
            return fn()
        except Exception as e:  # noqa: BLE001
            last = e
            if attempt < RETRIES - 1:
                time.sleep(RETRY_DELAY * (2 ** attempt))
    raise last


# ----------------------------------------------------------------------------
# 各后端实现
# ----------------------------------------------------------------------------

_EDGE_AUTH_URL = "https://edge.microsoft.com/translate/auth"
_EDGE_API_URL = "https://api-edge.cognitive.microsofttranslator.com/translate"
_BING_PAGE = "https://www.bing.com/translator"
_BING_TT = "https://www.bing.com/ttranslatev3"
_BAIDU_URL = "https://fanyi-api.baidu.com/api/trans/vip/translate"
_YOUDAO_URL = "https://openapi.youdao.com/api"

_bing_helper = None  # 进程内缓存：bing 页面的 abuse-prevention token


def _google_translate(text):
    params = urllib.parse.urlencode({
        "client": "gtx",
        "sl": _g_code(SRC),
        "tl": _g_code(DST),
        "dt": "t",
        "q": text,
    })
    url = "https://%s/translate_a/single?%s" % (GOOGLE_HOST, params)
    raw = _http_get(url)
    arr = json.loads(raw)
    parts = []
    for seg in arr[0]:
        if seg and seg[0]:
            parts.append(seg[0])
    return "".join(parts)


def _bing_get_helper():
    global _bing_helper
    if not _bing_helper:
        html = _http_get(_BING_PAGE, headers={"Referer": "https://www.bing.com/"})
        m = re.search(r'params_AbusePreventionHelper\s*=\s*"(.*?)"', html)
        if not m:
            raise TranslationError("无法从 bing 页面获取 token")
        _bing_helper = m.group(1)
    return _bing_helper


def _bing_translate(text):
    helper = _bing_get_helper()
    data = {
        "fromLang": "auto-detect" if SRC == "auto" else _bing_code(SRC),
        "to": _bing_code(DST),
        "text": text,
    }
    if "|" in helper:
        tok, key = helper.split("|", 1)
        data["token"] = tok
        data["key"] = key
    else:
        data["token"] = helper
    body = urllib.parse.urlencode(data).encode("utf-8")
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    raw = _http_post(_BING_TT, body, headers)
    arr = json.loads(raw)
    return arr[0]["translations"][0]["text"]


def _baidu_translate(text):
    if not BAIDU_APPID or not BAIDU_KEY:
        raise TranslationError("baidu 凭据缺失（请在 tr_config.ini 设置 baidu_appid/baidu_key）")
    salt = str(random.randint(100000, 999999))
    sign = hashlib.md5((BAIDU_APPID + text + salt + BAIDU_KEY).encode("utf-8")).hexdigest()
    params = {
        "q": text,
        "from": _baidu_code(SRC),
        "to": _baidu_code(DST),
        "appid": BAIDU_APPID,
        "salt": salt,
        "sign": sign,
    }
    url = "%s?%s" % (_BAIDU_URL, urllib.parse.urlencode(params))
    raw = _http_get(url)
    obj = json.loads(raw)
    if "trans_result" in obj and obj["trans_result"]:
        return "".join(r["dst"] for r in obj["trans_result"])
    raise TranslationError("baidu: %s" % obj.get("error_msg", raw))


def _youdao_translate(text):
    if not YOUDAO_APPKEY or not YOUDAO_APPSECRET:
        raise TranslationError("youdao 凭据缺失（请在 tr_config.ini 设置 youdao_appkey/youdao_appsecret）")
    salt = str(random.randint(1, 65536))
    curtime = str(int(time.time()))
    input_ = text if len(text) <= 20 else hashlib.sha256(text.encode("utf-8")).hexdigest()
    sign_str = YOUDAO_APPKEY + input_ + salt + curtime + YOUDAO_APPSECRET
    sign = hashlib.sha256(sign_str.encode("utf-8")).hexdigest()
    params = {
        "q": text,
        "from": _youdao_code(SRC),
        "to": _youdao_code(DST),
        "appKey": YOUDAO_APPKEY,
        "salt": salt,
        "sign": sign,
        "signType": "v3",
        "curtime": curtime,
    }
    url = "%s?%s" % (_YOUDAO_URL, urllib.parse.urlencode(params))
    raw = _http_get(url)
    obj = json.loads(raw)
    if obj.get("errorCode") == "0" and obj.get("translation"):
        return "".join(obj["translation"])
    raise TranslationError("youdao errorCode=%s" % obj.get("errorCode", "?"))


BACKENDS = {
    "google": _google_translate,
    "bing": _bing_translate,
    "baidu": _baidu_translate,
    "youdao": _youdao_translate,
}


def active_backends():
    """按用户指定顺序，过滤掉缺少凭据的后端。"""
    out = []
    for name in BACKEND_ORDER:
        if name not in BACKENDS:
            sys.stderr.write("[tr_engine] 未知后端: %s\n" % name)
            continue
        if name == "baidu" and (not BAIDU_APPID or not BAIDU_KEY):
            sys.stderr.write("[tr_engine] 跳过 baidu（未配置凭据）\n")
            continue
        if name == "youdao" and (not YOUDAO_APPKEY or not YOUDAO_APPSECRET):
            sys.stderr.write("[tr_engine] 跳过 youdao（未配置凭据）\n")
            continue
        out.append(name)
    return out


ACTIVE = active_backends()


# ----------------------------------------------------------------------------
# 磁盘缓存（跨会话复用，减少重复请求 / 频率限制）
# ----------------------------------------------------------------------------

CACHE_PATH = os.path.join(_script_dir(), "tr_cache.json")
DISK_CACHE = {}


def _load_disk_cache():
    global DISK_CACHE
    try:
        if os.path.exists(CACHE_PATH):
            with open(CACHE_PATH, "r", encoding="utf-8") as f:
                DISK_CACHE = json.load(f)
            if not isinstance(DISK_CACHE, dict):
                DISK_CACHE = {}
    except Exception:
        DISK_CACHE = {}


def _save_disk_cache():
    try:
        with open(CACHE_PATH, "w", encoding="utf-8") as f:
            json.dump(DISK_CACHE, f, ensure_ascii=False)
    except Exception as e:
        sys.stderr.write("[tr_engine] 写入缓存失败: %s\n" % e)


_load_disk_cache()


# ----------------------------------------------------------------------------
# 翻译一行
# ----------------------------------------------------------------------------

def translate_line(text):
    if text is None:
        return ""
    text = text.strip()
    if not text:
        return ""

    ckey = "%s|%s|%s" % (SRC, DST, text)
    if ckey in DISK_CACHE:
        return DISK_CACHE[ckey]

    if not ACTIVE:
        sys.stderr.write("[tr_engine] 没有可用后端（请检查 tr_config.ini 或网络）\n")
        return text  # 离线回退：原文

    last_err = None
    for name in ACTIVE:
        try:
            result = _with_retry(lambda n=name: BACKENDS[n](text))
            if result:
                DISK_CACHE[ckey] = result
                return result
        except Exception as e:  # noqa: BLE001
            last_err = e
            sys.stderr.write("[tr_engine] %s 失败: %s\n" % (name, e))
            continue

    sys.stderr.write("[tr_engine] 全部后端失败: %s... (%s)\n" % (text[:20], last_err))
    return text  # 全部失败，回退为原文（屏幕保留原文）


# ----------------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------------

def _read_lines(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read().split("\n")


def _write_lines(path, lines):
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    args = sys.argv[1:]

    # 调试：单行测试
    if args and args[0] == "--test":
        txt = args[1] if len(args) > 1 else "こんにちは、世界"
        print("后端顺序: %s" % (",".join(ACTIVE) or "(无)"))
        print("src=%s dst=%s" % (SRC, DST))
        print("原文: %s" % txt)
        print("译文: %s" % translate_line(txt))
        _save_disk_cache()
        return 0

    if args and args[0] == "--list":
        print("可用后端（按顺序，已过滤缺凭据的）: %s" % (",".join(ACTIVE) or "(无)"))
        print("SRC=%s DST=%s SLEEP=%s RETRIES=%s" % (SRC, DST, SLEEP, RETRIES))
        return 0

    if len(args) < 2:
        sys.stderr.write("usage: tr_engine.py <输入文件> <输出文件>\n")
        sys.stderr.write("       tr_engine.py --test \"原文\"\n")
        sys.stderr.write("       tr_engine.py --list\n")
        return 1

    infile, outfile = args[0], args[1]
    try:
        lines = _read_lines(infile)
    except Exception as e:
        sys.stderr.write("[tr_engine] 读取输入失败: %s\n" % e)
        return 1

    out = []
    for ln in lines:
        out.append(translate_line(ln))
        if SLEEP > 0:
            time.sleep(SLEEP)

    try:
        _write_lines(outfile, out)
    except Exception as e:
        sys.stderr.write("[tr_engine] 写入输出失败: %s\n" % e)
        return 1

    _save_disk_cache()
    return 0


if __name__ == "__main__":
    sys.exit(main())
