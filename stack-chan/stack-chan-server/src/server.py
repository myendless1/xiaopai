#!/usr/bin/env python3
import argparse
import base64
import datetime as _dt
import hashlib
import hmac
import json
import os
import random
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Empty

from morrow_client import MorrowClient
from morrow_coordinator import (
    DIALOGUE_COMMAND_PRIORITY,
    MorrowTurnCoordinator,
    command_store_reply_end_sink,
    command_store_segment_sink,
    parse_expression_tags,
)
from morrow_web import MorrowWebError, MorrowWebGateway
from realtime_server import RealtimeConfig, RealtimeManager
from realtime_protocol import ota_config
from yunet_service import YunetFaceService
from command_store import CommandStore
from database import Database
from device_registry import DeviceRegistry
from schemas import CommandEnvelope, DEFAULT_LEASE_MS, PROTOCOL_VERSION


ASR_URLS = {
    "shanghai": "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr",
    "beijing": "https://nls-gateway-cn-beijing.aliyuncs.com/stream/v1/asr",
    "shenzhen": "https://nls-gateway-cn-shenzhen.aliyuncs.com/stream/v1/asr",
}

TTS_URLS = {
    "shanghai": "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/tts",
    "beijing": "https://nls-gateway-cn-beijing.aliyuncs.com/stream/v1/tts",
    "shenzhen": "https://nls-gateway-cn-shenzhen.aliyuncs.com/stream/v1/tts",
}
DEFAULT_TTS_VOICE = "zhimiao_emo"
DEFAULT_OTA_FIRMWARE_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "static", "firmware")

TOKEN_META_ENDPOINT = "https://nls-meta.cn-shanghai.aliyuncs.com/"
TOKEN_REGION_ID = "cn-shanghai"
TOKEN_API_VERSION = "2019-02-28"
TOKEN_REFRESH_MARGIN_SECONDS = 300
DEVICE_ONLINE_TTL_SECONDS = 90
DIALOG_AWAKE_SECONDS = 180
DEFAULT_OTA_CHECK_INTERVAL_SECONDS = 300
DEFAULT_SEDENTARY_REMINDER_INTERVAL_SECONDS = 30 * 60
LOG_TEXT_MAX_CHARS = 2000
DEVICE_LOG_MAX_ITEMS = 500
DEVICE_RECORDING_MAX_ITEMS = 100
DIALOG_WAKE_WORDS = (
    "小派同学",
    "小派同學",
    "小派",
    "小胖",
    "小盼",
    "小潘",
    "小排",
    "小白",
    "小坏",
    "小壞",
    "小蔡同学",
    "小蔡同學",
    "小蔡",
    "小的同学",
    "小的同學",
    "小的",
    "小外",
    "小机器",
    "机器人",
    "小盘",
    "小泡",
    "xiaopai",
)
DIALOG_SLEEP_BYE_WORDS = (
    "拜拜",
    "再见",
    "再會",
    "再会",
)
DIALOG_SLEEP_REST_WORDS = (
    "退下吧",
    "退下",
    "退一下",
    "退一下吧",
    "退一退",
    "休息",
    "睡觉",
    "睡覺",
    "睡眠",
    "先这样",
    "先這樣",
)
DIALOG_SLEEP_WORDS = DIALOG_SLEEP_REST_WORDS + DIALOG_SLEEP_BYE_WORDS
DIALOG_WAKE_ONLY_FILLERS = (
    "你好",
    "您好",
    "同学",
    "同學",
    "在吗",
    "在嗎",
    "醒醒",
    "hello",
    "hi",
    "嗨",
    "哈喽",
    "哈囉",
)
SUPPRESSED_FALLBACK_SPEECH_NORMALIZED = {
    "\u6211\u6ca1\u542c\u6e05\u53ef\u4ee5\u518d\u8bf4\u4e00\u904d\u5417",
}

AVAILABLE_EXPRESSIONS = (
    "calm",
    "shy",
    "happy",
    "thinking",
    "surprised",
    "sleep_dark",
    "screen_off",
)

AVAILABLE_ACTIONS = (
    "blink",
    "wink",
    "heart_action",
    "hearting",
    "nod",
    "nodding",
    "happy_dynamic",
    "happy_squint_dynamic",
    "node_head",
    "nod_head",
)

PHYSICAL_ACTIONS = {"node_head", "nod_head"}

COMMAND_QUEUE_MAX_SIZE = 24
SPEAKER_VOLUME_MIN = 5
SPEAKER_VOLUME_MAX = 100
SPEAKER_VOLUME_DEFAULT = 10
COMMAND_DEFAULT_PRIORITIES = {
    "stop": 100,
    "volume": 90,
    "sound": 90,
    "state": 88,
    "device_state": 88,
    "find_owner": 85,
    "locate_owner": 85,
    "capture_image": 70,
    "track_once": 70,
    "camera": 70,
    "face": 65,
    "expression": 65,
    "action": 65,
    "node_head": 45,
    "nod_head": 45,
    "motion": 45,
    "move": 45,
    "sequence": 30,
    "check_ota": 25,
    "ota_check": 25,
    "firmware_ota": 25,
    "speak": 10,
    "play_audio": 10,
}
COMMAND_DEFAULT_TTL_SECONDS = {
    "state": 8.0,
    "device_state": 8.0,
    "face": 8.0,
    "expression": 8.0,
    "action": 8.0,
    "node_head": 5.0,
    "nod_head": 5.0,
    "motion": 5.0,
    "move": 5.0,
    "speak": 30.0,
    "sequence": 45.0,
    "check_ota": 600.0,
    "ota_check": 600.0,
    "firmware_ota": 600.0,
}
COMMAND_COALESCE_BY_TYPE = {"state", "device_state", "face", "expression", "action", "node_head", "nod_head", "motion", "move", "speak", "check_ota", "ota_check", "firmware_ota"}
COMMAND_DISCARDABLE_TYPES = {"state", "device_state", "face", "expression", "action", "node_head", "nod_head", "motion", "move", "speak", "sequence", "check_ota", "ota_check", "firmware_ota"}

WAKE_REPLY_EVENTS = (
    ("wake_reply", "我在。"),
    ("wake_reply_help", "有什么要帮忙的"),
    ("wake_reply_hello", "你好呀"),
    ("wake_reply_here", "我在呢"),
    ("wake_reply_xiaopai_here", "小派在呢"),
)
SLEEP_REPLY_BYE_EVENTS = (
    ("sleep_reply_bye", "拜拜"),
    ("sleep_reply_goodbye", "再见"),
)
SLEEP_REPLY_REST_EVENTS = (
    ("sleep_reply_ok", "好的"),
    ("sleep_reply_ok_master", "好的主人"),
    ("sleep_reply_bye", "拜拜"),
    ("sleep_reply_obey", "遵命"),
)
SLEEP_REPLY_EVENTS = tuple({name: text for name, text in SLEEP_REPLY_BYE_EVENTS + SLEEP_REPLY_REST_EVENTS}.items())
SEDENTARY_REMINDER_EVENTS = (
    ("sedentary_reminder_stretch", "你已连续工作好长时间啦，起身拉伸一下吧。"),
    ("sedentary_reminder_move", "小派观察到你一直在忙，站起来活动两分钟吧。"),
    ("sedentary_reminder_walk", "眼睛和肩颈都需要休息一下，起身走一走吧。"),
)
TRAVEL_REMINDER_EVENTS = (
    ("travel_packing_reminder", "记得增添外套，带好雨具。同时提醒您记得携带身份证、充电器、出差办公资料~"),
    ("travel_formalwear_reminder", "查询到日程备注栏写着建议着正装，建议您带上一套，祝您一路平安！回来后见~"),
)
PREWARM_EVENT_AUDIO_NAMES = tuple(
    name for name, _text in WAKE_REPLY_EVENTS + SLEEP_REPLY_EVENTS + SEDENTARY_REMINDER_EVENTS + TRAVEL_REMINDER_EVENTS
)
EVENT_AUDIO_CACHE_META_VERSION = 2
ESP_APP_DESC_MAGIC_WORD = 0xABCD5432
ESP_IMAGE_HEADER_SIZE = 24
ESP_IMAGE_SEGMENT_HEADER_SIZE = 8
ESP_APP_DESC_OFFSET = ESP_IMAGE_HEADER_SIZE + ESP_IMAGE_SEGMENT_HEADER_SIZE
ESP_APP_DESC_VERSION_OFFSET = ESP_APP_DESC_OFFSET + 16
ESP_APP_DESC_PROJECT_NAME_OFFSET = ESP_APP_DESC_VERSION_OFFSET + 32

HEAD_TOUCH_EVENT_TEXT = {name: text for name, text in WAKE_REPLY_EVENTS}
HEAD_TOUCH_EVENT_TEXT.update({name: text for name, text in SLEEP_REPLY_EVENTS})
HEAD_TOUCH_EVENT_TEXT.update(
    {
        "press": "按压",
        "click": "你好，我是小派同学",
        "swipe_forward": "你好，我是小派同学",
        "swipe_backward": "你好，我是小派同学",
    }
)
EVENT_AUDIO_TEXT = dict(HEAD_TOUCH_EVENT_TEXT)
EVENT_AUDIO_TEXT.update({name: text for name, text in SEDENTARY_REMINDER_EVENTS})
EVENT_AUDIO_TEXT.update({name: text for name, text in TRAVEL_REMINDER_EVENTS})
AUTO_SPEECH_CACHE_BY_TEXT = {text: name for name, text in TRAVEL_REMINDER_EVENTS}

ALIYUN_TTS_VOICE_DOC_URL = "https://help.aliyun.com/zh/isi/developer-reference/overview-of-speech-synthesis"
ALIYUN_TTS_DEBUG_VOICES = (
    {"name": "小云", "voice": "xiaoyun", "type": "标准女声", "language": "中文/中英混合"},
    {"name": "小刚", "voice": "xiaogang", "type": "标准男声", "language": "中文/中英混合"},
    {"name": "小美", "voice": "xiaomei", "type": "甜美女声", "language": "中文/中英混合"},
    {"name": "若兮", "voice": "ruoxi", "type": "温柔女声", "language": "中文/中英混合"},
    {"name": "思琪", "voice": "siqi", "type": "温柔女声", "language": "中文/中英混合"},
    {"name": "思佳", "voice": "sijia", "type": "标准女声", "language": "中文/中英混合"},
    {"name": "思诚", "voice": "sicheng", "type": "标准男声", "language": "中文/中英混合"},
    {"name": "艾琪", "voice": "aiqi", "type": "温柔女声", "language": "中文/中英混合"},
    {"name": "艾佳", "voice": "aijia", "type": "标准女声", "language": "中文/中英混合"},
    {"name": "艾诚", "voice": "aicheng", "type": "标准男声", "language": "中文/中英混合"},
    {"name": "艾达", "voice": "aida", "type": "标准男声", "language": "中文/中英混合"},
    {"name": "宁儿", "voice": "ninger", "type": "标准女声", "language": "纯中文"},
    {"name": "瑞琳", "voice": "ruilin", "type": "标准女声", "language": "纯中文"},
    {"name": "思悦", "voice": "siyue", "type": "温柔女声", "language": "中文/中英混合"},
    {"name": "艾雅", "voice": "aiya", "type": "严厉女声", "language": "中文/中英混合"},
    {"name": "艾美", "voice": "aimei", "type": "甜美女声", "language": "中文/中英混合"},
    {"name": "艾雨", "voice": "aiyu", "type": "自然女声", "language": "中文/中英混合"},
    {"name": "艾悦", "voice": "aiyue", "type": "温柔女声", "language": "中文/中英混合"},
    {"name": "艾婧", "voice": "aijing", "type": "严厉女声", "language": "中文/中英混合"},
    {"name": "思彤", "voice": "sitong", "type": "儿童音", "language": "纯中文"},
    {"name": "小北", "voice": "xiaobei", "type": "萝莉女声", "language": "纯中文"},
    {"name": "艾彤", "voice": "aitong", "type": "儿童音", "language": "纯中文"},
    {"name": "艾薇", "voice": "aiwei", "type": "萝莉女声", "language": "纯中文"},
    {"name": "艾宝", "voice": "aibao", "type": "萝莉女声", "language": "纯中文"},
    {"name": "知小白", "voice": "zhixiaobai", "type": "普通话女声", "language": "中文/中英混合"},
    {"name": "知小夏", "voice": "zhixiaoxia", "type": "普通话女声", "language": "中文/中英混合"},
    {"name": "知小妹", "voice": "zhixiaomei", "type": "普通话女声", "language": "中文/中英混合"},
    {"name": "知硕", "voice": "zhishuo", "type": "普通话男声", "language": "中文/中英混合"},
    {"name": "知锋_多情感", "voice": "zhifeng_emo", "type": "多情感男声", "language": "中文/中英混合"},
    {"name": "知冰_多情感", "voice": "zhibing_emo", "type": "多情感男声", "language": "纯中文"},
    {"name": "知妙_多情感", "voice": "zhimiao_emo", "type": "多情感女声", "language": "中文/英文"},
    {"name": "知米_多情感", "voice": "zhimi_emo", "type": "多情感女声", "language": "中文/中英混合"},
    {"name": "知燕_多情感", "voice": "zhiyan_emo", "type": "多情感女声", "language": "中文/中英混合"},
    {"name": "知贝_多情感", "voice": "zhibei_emo", "type": "多情感童声", "language": "中文/中英混合"},
    {"name": "知甜_多情感", "voice": "zhitian_emo", "type": "多情感女声", "language": "中文/中英混合"},
    {"name": "Harry", "voice": "harry", "type": "英音男声", "language": "英文"},
    {"name": "Abby", "voice": "abby", "type": "美音女声", "language": "英文"},
    {"name": "Cally", "voice": "cally", "type": "美式英文女声", "language": "英文"},
)

def log_timestamp() -> str:
    return _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def log_print(message: str, *, file=None) -> None:
    print(f"[{log_timestamp()}] {message}", file=file or sys.stdout, flush=True)

EXPRESSION_ALIASES = {
    "default": "calm",
    "listening": "calm",
    "stopped": "sleep_dark",
    "sleep": "sleep_dark",
    "screen_off": "sleep_dark",
    "think": "thinking",
    "thinking": "thinking",
    "relax": "relaxed",
    "relaxed": "relaxed",
    "smile_blink": "smile_blink",
    "heart": "heart_action",
    "love": "heart_action",
    "wink": "wink",
    "blink": "blink",
    "shy": "shy",
    "happy": "happy",
    "surprised": "surprised",
    "calm": "calm",
    "开心": "happy",
    "惊讶": "surprised",
    "微笑眨眼": "smile_blink",
    "眨眼微笑": "smile_blink",
    "舒缓": "relaxed",
    "舒缓轻松": "relaxed",
    "放松": "relaxed",
    "暗屏": "sleep_dark",
    "休眠": "sleep_dark",
    "害羞": "shy",
    "爱心": "heart_action",
    "思考": "thinking",
    "眨眼": "wink",
    "点头": "nod",
    "物理点头": "node_head",
    "头部点头": "node_head",
    "node_head": "node_head",
    "nod_head": "nod_head",
}

MOTION_DIRECTION_ALIASES = {
    "左": "left",
    "左边": "left",
    "左转": "left",
    "向左": "left",
    "往左": "left",
    "朝左": "left",
    "转左": "left",
    "右": "right",
    "右边": "right",
    "右转": "right",
    "向右": "right",
    "往右": "right",
    "朝右": "right",
    "转右": "right",
    "上": "up",
    "上面": "up",
    "向上": "up",
    "往上": "up",
    "朝上": "up",
    "抬头": "up",
    "下": "down",
    "下面": "down",
    "向下": "down",
    "往下": "down",
    "朝下": "down",
    "低头": "down",
    "left": "left",
    "right": "right",
    "up": "up",
    "down": "down",
}

MOTION_CENTER_PHRASES = (
    "请回正",
    "回正",
    "回中",
    "回中间",
    "回到中间",
    "回到正中",
    "回到正中间",
    "回到初始位置",
    "回到初始",
    "回初始位置",
    "回初始",
    "恢复初始位置",
    "恢复初始",
    "归位",
    "复位",
    "重置位置",
    "回家",
    "center",
    "home",
)

VOICE_FACE_COMMAND_TRIGGERS = (
    "切换到",
    "切到",
    "换成",
    "切换",
    "显示",
    "设置为",
    "设为",
    "变成",
    "做",
    "表情",
    "动作",
    "expression",
    "face",
    "action",
)

VOICE_FACE_ALIASES = (
    ("heart_action", ("爱心", "吐爱心", "亲亲爱心", "heart action", "hearting", "love")),
    ("wink", ("眨眼", "眨一下眼", "单眼眨眼", "wink")),
    ("thinking", ("思考", "思考表情", "想一想", "想一下", "thinking", "think")),
    ("happy_squint_soft", ("眯眼笑", "眯眼微笑", "happy squint soft", "happy_squint_soft", "柔和眯眼笑", "柔和眯眼开心")),
    ("happy_squint", ("开心表情", "开心", "高兴表情", "高兴", "快乐表情", "快乐", "happy squint", "happy_squint", "happy")),
    ("smile_blink", ("眨眼微笑", "微笑眨眼", "smile blink", "smile_blink")),
    ("relaxed", ("舒缓轻松", "舒缓", "放松表情", "放松", "relaxed", "relax")),
    ("calm", ("平静表情", "平静", "冷静表情", "冷静", "calm")),
    ("shy", ("害羞表情", "害羞", "羞涩表情", "羞涩", "shy")),
)

JOKE_TEXT_XIAOMING_SLOW_SCHOOL = (
    "老师问小明：“你为什么总是迟到？”\n"
    "小明说：“因为路上有个牌子写着‘学校前方，请慢行’。”\n"
    "老师气笑了：“那你也不能慢成这样吧？”\n"
    "小明委屈地说：“我已经很努力了，今天还超速了两步。”"
)

VOICE_SPEAK_COMMANDS = (
    {
        "name": "joke_xiaoming_slow_school",
        "aliases": ("讲个笑话", "说个笑话", "来个笑话", "讲笑话"),
        "text": JOKE_TEXT_XIAOMING_SLOW_SCHOOL,
    },
)

CHINESE_DIGITS = {
    "零": 0,
    "〇": 0,
    "一": 1,
    "二": 2,
    "两": 2,
    "三": 3,
    "四": 4,
    "五": 5,
    "六": 6,
    "七": 7,
    "八": 8,
    "九": 9,
}


def split_sentences(text: str, max_chars: int):
    text = re.sub(r"\s+", " ", text.strip())
    if not text:
        return

    buf = []
    for ch in text:
        buf.append(ch)
        sentence_end = ch in "。！？!?；;\n"
        if sentence_end or len(buf) >= max_chars:
            part = "".join(buf).strip()
            if part:
                yield part
            buf.clear()

    part = "".join(buf).strip()
    if part:
        yield part


def detect_wav_sample_rate(data: bytes) -> int | None:
    if len(data) < 28 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    return struct.unpack_from("<I", data, 24)[0]


def read_binary_file(path: str) -> bytes:
    with open(path, "rb") as fp:
        return fp.read()


def pcm_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    data_size = len(pcm)
    byte_rate = sample_rate * 2
    return b"".join(
        (
            b"RIFF",
            struct.pack("<I", 36 + data_size),
            b"WAVE",
            b"fmt ",
            struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, byte_rate, 2, 16),
            b"data",
            struct.pack("<I", data_size),
            pcm,
        )
    )


def save_audio_capture(
    audio: bytes,
    *,
    capture_dir: str,
    device_id: str,
    prefix: str,
    audio_format: str,
    sample_rate: int,
) -> str:
    if not audio or not capture_dir:
        return ""
    os.makedirs(capture_dir, exist_ok=True)
    safe_device = re.sub(r"[^A-Za-z0-9_.-]+", "_", safe_device_id(device_id))[:40] or "unknown"
    safe_prefix = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(prefix or "upload"))[:48] or "upload"
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    path = os.path.join(capture_dir, f"{safe_prefix}-{safe_device}-{stamp}.wav")
    data = audio if audio_format == "wav" else pcm_to_wav(audio, sample_rate)
    with open(path, "wb") as fp:
        fp.write(data)
    return path


def wav_data_payload(data: bytes) -> bytes:
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return data
    offset = 12
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        payload_start = offset + 8
        payload_end = min(len(data), payload_start + chunk_size)
        if chunk_id == b"data":
            return data[payload_start:payload_end]
        offset = payload_start + chunk_size + (chunk_size & 1)
    return b""


def save_wav_raw_sidecar(wav_path: str, audio: bytes) -> str:
    if not wav_path:
        return ""
    raw = wav_data_payload(audio)
    if not raw:
        return ""
    raw_path = os.path.splitext(wav_path)[0] + ".pcm"
    with open(raw_path, "wb") as fp:
        fp.write(raw)
    return raw_path


def parse_chinese_integer(text: str) -> int | None:
    text = text.strip()
    if not text:
        return None
    if all(ch in CHINESE_DIGITS for ch in text):
        value = 0
        for ch in text:
            value = value * 10 + CHINESE_DIGITS[ch]
        return value

    total = 0
    current = 0
    for ch in text:
        if ch in CHINESE_DIGITS:
            current = CHINESE_DIGITS[ch]
        elif ch == "十":
            total += (current or 1) * 10
            current = 0
        elif ch == "百":
            total += (current or 1) * 100
            current = 0
        else:
            return None
    return total + current


def parse_spoken_number(text: str) -> float | None:
    text = text.strip().translate(str.maketrans("０１２３４５６７８９．", "0123456789."))
    if not text:
        return None
    if re.fullmatch(r"\d+(?:\.\d+)?", text):
        return float(text)
    value = parse_chinese_integer(text)
    return float(value) if value is not None else None


def parse_voice_motion_command(text: str) -> dict | None:
    normalized = re.sub(r"[\s,，。.!！?？]+", "", text.strip().lower())
    if not normalized:
        return None

    if any(phrase in normalized for phrase in MOTION_CENTER_PHRASES):
        return {
            "type": "center",
            "duration_ms": 600,
            "source_text": text,
        }

    number_pattern = r"([0-9０-９]+(?:[.．][0-9０-９]+)?|[零〇一二两三四五六七八九十百]+)"
    direction_pattern = (
        r"(左转|右转|转左|转右|向左|向右|往左|往右|朝左|朝右|左边|右边|"
        r"抬头|低头|向上|向下|往上|往下|朝上|朝下|上面|下面|左|右|上|下|"
        r"left|right|up|down)"
    )
    action_pattern = r"(?:转|转动|移动|动|摆|看|运动)?"
    patterns = (
        re.compile(direction_pattern + action_pattern + number_pattern + r"(?:度|degrees?|°)"),
        re.compile(number_pattern + r"(?:度|degrees?|°)" + action_pattern + direction_pattern),
    )

    for pattern in patterns:
        match = pattern.search(normalized)
        if not match:
            continue
        first, second = match.group(1), match.group(2)
        if first in MOTION_DIRECTION_ALIASES:
            direction_text, number_text = first, second
        else:
            number_text, direction_text = first, second
        degree = parse_spoken_number(number_text)
        direction = MOTION_DIRECTION_ALIASES.get(direction_text)
        if direction and degree is not None and degree > 0:
            return {
                "type": direction,
                "degree": degree,
                "duration_ms": 500,
                "source_text": text,
            }
    return None


def normalize_voice_command_text(text: str) -> str:
    return re.sub(r"[\s,_\-，。.!！?？/（）()]+", "", text.strip().lower())


def speech_text_is_temporarily_suppressed(text: str) -> bool:
    return normalize_voice_command_text(str(text or "")) in SUPPRESSED_FALLBACK_SPEECH_NORMALIZED


SPEECH_ENDING_PUNCT_RE = re.compile(r"[。！？!?；;]$")
MARKDOWN_TABLE_SEPARATOR_CELL_RE = re.compile(r"^:?-{3,}:?$")
DISPLAY_SYMBOL_RE = re.compile(r"[✅✔☑❌✖\U0001F300-\U0001FAFF\U00002700-\U000027BF]")


def normalize_speech_text_for_voice(text: str) -> str:
    value = strip_markdown_syntax(normalize_markdown_tables(str(text or "")))
    value = value.replace("\r\n", "\n").replace("\r", "\n").replace("\u00a0", " ")
    value = DISPLAY_SYMBOL_RE.sub("", value)
    value = re.sub(r"[ \t]*\n[ \t]*", " ", value)
    value = re.sub(r"[ \t]+", " ", value)
    value = re.sub(r"\s*([，。！？、；：])\s*", r"\1", value)
    value = re.sub(r"\s+([,.!?;:])", r"\1", value)
    return value.strip()


def normalize_markdown_tables(text: str) -> str:
    prepared = re.sub(r"\|\s+(?=\|)", "|\n", text.replace("\r\n", "\n").replace("\r", "\n"))
    lines = prepared.split("\n")
    output: list[str] = []
    index = 0
    while index < len(lines):
        original_line = lines[index]
        next_line = lines[index + 1].strip() if index + 1 < len(lines) else ""
        prefix, table_line = split_table_prefix(original_line, next_line)

        if is_markdown_table_row(table_line) and is_markdown_table_separator_row(next_line):
            if prefix:
                output.append(prefix)
            headers = split_markdown_table_row(table_line)
            rows: list[str] = []
            index += 2
            while index < len(lines):
                row_line = lines[index].strip()
                if not is_markdown_table_row(row_line) or is_markdown_table_separator_row(row_line):
                    break
                row = format_markdown_table_row(headers, split_markdown_table_row(row_line))
                if row:
                    rows.append(row)
                index += 1
            if rows:
                output.append(with_sentence_ending("；".join(rows)))
            continue

        output.append(original_line)
        index += 1
    return "\n".join(output)


def split_table_prefix(line: str, next_line: str) -> tuple[str, str]:
    trimmed = line.strip()
    pipe_index = trimmed.find("|")
    if pipe_index <= 0:
        return "", trimmed
    candidate = trimmed[pipe_index:].strip()
    if not is_markdown_table_row(candidate) or not is_markdown_table_separator_row(next_line):
        return "", trimmed
    return trimmed[:pipe_index].strip(), candidate


def is_markdown_table_row(line: str) -> bool:
    trimmed = line.strip()
    return trimmed.startswith("|") and trimmed.endswith("|") and len(split_markdown_table_row(trimmed)) >= 2


def is_markdown_table_separator_row(line: str) -> bool:
    cells = split_markdown_table_row(line)
    return len(cells) >= 2 and all(MARKDOWN_TABLE_SEPARATOR_CELL_RE.match(cell.replace(" ", "")) for cell in cells)


def split_markdown_table_row(line: str) -> list[str]:
    return [clean_markdown_cell(cell) for cell in line.strip().strip("|").split("|")]


def clean_markdown_cell(value: str) -> str:
    value = value.strip()
    value = re.sub(r"(\*\*|__)(.*?)\1", r"\2", value)
    value = re.sub(r"~~(.*?)~~", r"\1", value)
    value = re.sub(r"`([^`]+)`", r"\1", value)
    value = re.sub(r"[*_`]+", "", value)
    return re.sub(r"\s+", " ", value).strip()


def format_markdown_table_row(headers: list[str], cells: list[str]) -> str:
    parts: list[str] = []
    for index in range(max(len(headers), len(cells))):
        cell = cells[index].strip() if index < len(cells) else ""
        if not cell:
            continue
        header = headers[index].strip() if index < len(headers) else ""
        if not header or is_header_safe_to_omit(header):
            parts.append(cell)
        else:
            parts.append(f"{header}{cell}")
    return "，".join(parts)


def is_header_safe_to_omit(header: str) -> bool:
    return bool(re.match(r"^(时间|日期|时段|开始|结束|内容|事项|标题|名称|事件|日程)$", re.sub(r"\s+", "", header), re.I))


def with_sentence_ending(value: str) -> str:
    value = value.strip()
    if not value or SPEECH_ENDING_PUNCT_RE.search(value):
        return value
    return f"{value}。"


def strip_markdown_syntax(text: str) -> str:
    value = re.sub(r"```[A-Za-z0-9_-]*\n?", "", text)
    value = value.replace("```", "")
    value = re.sub(r"!\[([^\]]*)\]\([^)]+\)", r"\1", value)
    value = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", value)
    value = re.sub(r"`([^`]+)`", r"\1", value)
    value = "\n".join(strip_markdown_line_prefix(line) for line in value.split("\n"))
    value = re.sub(r"(\*\*|__)(.*?)\1", r"\2", value)
    value = re.sub(r"~~(.*?)~~", r"\1", value)
    value = re.sub(r"(^|[^\w])\*([^*\n]+)\*", r"\1\2", value)
    value = re.sub(r"(^|[^\w])_([^_\n]+)_", r"\1\2", value)
    value = re.sub(r"^[|:\-\s]+$", "", value, flags=re.MULTILINE)
    value = re.sub(r"\s*\|\s*", "，", value)
    return re.sub(r"[*_`]+", "", value)


def strip_markdown_line_prefix(line: str) -> str:
    line = line.strip()
    line = re.sub(r"^#{1,6}\s+", "", line)
    line = re.sub(r"^>\s?", "", line)
    line = re.sub(r"^[-*+]\s+", "", line)
    return re.sub(r"^\d+[.)]\s+", "", line)


def has_dialog_wake_word(text: str) -> bool:
    normalized = normalize_voice_command_text(text)
    return any(normalize_voice_command_text(word) in normalized for word in DIALOG_WAKE_WORDS)


def has_dialog_sleep_word(text: str) -> bool:
    normalized = normalize_voice_command_text(text)
    return any(normalize_voice_command_text(word) in normalized for word in DIALOG_SLEEP_WORDS)


def sleep_reply_event_for_text(text: str) -> tuple[str, str]:
    normalized = normalize_voice_command_text(text)
    if any(normalize_voice_command_text(word) in normalized for word in DIALOG_SLEEP_BYE_WORDS):
        return random.choice(SLEEP_REPLY_BYE_EVENTS)
    if any(normalize_voice_command_text(word) in normalized for word in DIALOG_SLEEP_REST_WORDS):
        return random.choice(SLEEP_REPLY_REST_EVENTS)
    return random.choice(SLEEP_REPLY_REST_EVENTS)


def is_wake_only_text(text: str) -> bool:
    normalized = normalize_voice_command_text(text)
    for word in DIALOG_WAKE_WORDS:
        normalized = normalized.replace(normalize_voice_command_text(word), "")
    for filler in DIALOG_WAKE_ONLY_FILLERS:
        normalized = normalized.replace(normalize_voice_command_text(filler), "")
    return not normalized


def parse_voice_volume_command(text: str) -> dict | None:
    normalized = normalize_voice_command_text(text)
    if "声音" not in normalized:
        return None
    down_words = ("小", "轻", "低", "降低", "调低", "关小", "小点", "小一点")
    up_words = ("大", "响", "高", "提高", "调高", "放大", "大点", "大一点")
    if any(word in normalized for word in down_words):
        if "最" in normalized:
            return {"mode": "set", "value": 10, "source_text": text}
        return {"direction": "down", "step": 10, "source_text": text}
    if any(word in normalized for word in up_words):
        if "最" in normalized:
            return {"mode": "set", "value": 100, "source_text": text}
        return {"direction": "up", "step": 10, "source_text": text}
    return None


def parse_voice_face_command(text: str) -> dict | None:
    normalized = normalize_voice_command_text(text)
    if not normalized:
        return None

    has_trigger = any(trigger in normalized for trigger in VOICE_FACE_COMMAND_TRIGGERS)
    for expression, aliases in VOICE_FACE_ALIASES:
        for alias in aliases:
            alias_normalized = normalize_voice_command_text(alias)
            if not alias_normalized:
                continue
            if normalized == alias_normalized or (has_trigger and alias_normalized in normalized):
                return {
                    "expression": expression,
                    "source_text": text,
                }
    return None


def parse_voice_speak_command(text: str) -> dict | None:
    normalized = normalize_voice_command_text(text)
    if not normalized:
        return None

    for command in VOICE_SPEAK_COMMANDS:
        for alias in command["aliases"]:
            alias_normalized = normalize_voice_command_text(alias)
            if normalized == alias_normalized or alias_normalized in normalized:
                return {
                    "name": command["name"],
                    "text": command["text"],
                    "source_text": text,
                }
    return None


@dataclass(frozen=True)
class TtsRequestOptions:
    voice: str
    sample_rate: int
    volume: int
    speech_rate: int
    pitch_rate: int
    audio_format: str = "pcm"


def parse_int_range(value, *, default: int, name: str, min_value: int, max_value: int) -> int:
    if value is None or value == "":
        return default
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < min_value or parsed > max_value:
        raise ValueError(f"{name} must be between {min_value} and {max_value}")
    return parsed


def tts_request_options_from_params(server, params: dict) -> TtsRequestOptions:
    configured_voice = str(getattr(server, "voice", DEFAULT_TTS_VOICE) or DEFAULT_TTS_VOICE).strip()
    requested_voice = str(params.get("voice") or "").strip()
    # Firmware sends the sentinel "default" when no explicit voice was
    # selected. It is not an Aliyun voice ID; resolve it locally.
    voice = configured_voice if not requested_voice or requested_voice.lower() == "default" else requested_voice
    if not voice:
        raise ValueError("voice must not be empty")
    audio_format = str(params.get("format") or params.get("audio_format") or "pcm").strip().lower().lstrip(".")
    if audio_format in ("raw", "s16le", "pcm_s16le"):
        audio_format = "pcm"
    if audio_format not in ("pcm", "wav"):
        raise ValueError("format must be pcm or wav")
    return TtsRequestOptions(
        voice=voice,
        sample_rate=parse_int_range(
            params.get("sample_rate"),
            default=int(getattr(server, "sample_rate", 24000)),
            name="sample_rate",
            min_value=8000,
            max_value=48000,
        ),
        volume=parse_int_range(
            params.get("volume"),
            default=int(getattr(server, "volume", 80)),
            name="volume",
            min_value=0,
            max_value=100,
        ),
        speech_rate=parse_int_range(
            params.get("speech_rate"),
            default=int(getattr(server, "speech_rate", 0)),
            name="speech_rate",
            min_value=-500,
            max_value=500,
        ),
        pitch_rate=parse_int_range(
            params.get("pitch_rate"),
            default=int(getattr(server, "pitch_rate", 0)),
            name="pitch_rate",
            min_value=-500,
            max_value=500,
        ),
        audio_format=audio_format,
    )


@dataclass(frozen=True)
class OtaFirmwareInfo:
    path: str
    version: str
    project_name: str
    size: int
    mtime: float
    sha256: str


def _read_c_string(blob: bytes) -> str:
    value = blob.split(b"\x00", 1)[0]
    return value.decode("utf-8", errors="replace").strip()


def is_ota_version(version: str) -> bool:
    return bool(re.fullmatch(r"\d+(?:\.\d+)*", str(version or "").strip()))


def ota_version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in str(version or "0").split(".") if part != "")


def parse_esp_app_firmware_info(path: str) -> OtaFirmwareInfo | None:
    try:
        stat = os.stat(path)
        with open(path, "rb") as fp:
            header = fp.read(ESP_APP_DESC_PROJECT_NAME_OFFSET + 32)
            fp.seek(0)
            digest = hashlib.sha256()
            while True:
                chunk = fp.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError:
        return None

    if len(header) < ESP_APP_DESC_PROJECT_NAME_OFFSET + 32:
        return None
    magic = struct.unpack_from("<I", header, ESP_APP_DESC_OFFSET)[0]
    if magic != ESP_APP_DESC_MAGIC_WORD:
        return None

    version = _read_c_string(header[ESP_APP_DESC_VERSION_OFFSET : ESP_APP_DESC_VERSION_OFFSET + 32])
    project_name = _read_c_string(header[ESP_APP_DESC_PROJECT_NAME_OFFSET : ESP_APP_DESC_PROJECT_NAME_OFFSET + 32])
    if not version:
        return None
    return OtaFirmwareInfo(
        path=os.path.abspath(path),
        version=version,
        project_name=project_name,
        size=stat.st_size,
        mtime=stat.st_mtime,
        sha256=digest.hexdigest(),
    )


def find_latest_ota_firmware(firmware_file: str, firmware_dir: str) -> OtaFirmwareInfo | None:
    if firmware_file:
        info = parse_esp_app_firmware_info(firmware_file)
        if info is None or not is_ota_version(info.version):
            return None
        return info
    if not firmware_dir or not os.path.isdir(firmware_dir):
        return None

    candidates: list[OtaFirmwareInfo] = []
    for root, _dirs, files in os.walk(firmware_dir):
        for name in files:
            if not name.endswith(".bin"):
                continue
            if name in ("bootloader.bin", "partition-table.bin"):
                continue
            info = parse_esp_app_firmware_info(os.path.join(root, name))
            if info is not None and is_ota_version(info.version):
                candidates.append(info)
    if not candidates:
        return None
    return max(candidates, key=lambda item: (ota_version_key(item.version), item.mtime, item.path))


def ota_firmware_public_url(server, request_headers, firmware: OtaFirmwareInfo) -> str:
    base_url = str(getattr(server, "ota_public_base_url", "") or "").rstrip("/")
    if not base_url:
        host = request_headers.get("Host", "")
        if not host:
            host = f"127.0.0.1:{getattr(server, 'server_port', 80)}"
        base_url = f"http://{host}"
    filename = urllib.parse.quote(os.path.basename(firmware.path))
    return f"{base_url}/firmware/{filename}"


def ota_firmware_manifest(server, request_headers, firmware: OtaFirmwareInfo) -> dict:
    return {
        "version": firmware.version,
        "url": ota_firmware_public_url(server, request_headers, firmware),
        "size": firmware.size,
        "sha256": firmware.sha256,
        "project_name": firmware.project_name,
        "updated_at": _dt.datetime.fromtimestamp(firmware.mtime).isoformat(timespec="seconds"),
    }


def command_default_priority(command_type: str) -> int:
    return COMMAND_DEFAULT_PRIORITIES.get(str(command_type or ""), 20)


def command_default_ttl(command_type: str) -> float:
    return COMMAND_DEFAULT_TTL_SECONDS.get(str(command_type or ""), 60.0)


def command_is_discardable(command: dict) -> bool:
    if "discardable" in command:
        return bool(command.get("discardable"))
    return str(command.get("type") or "") in COMMAND_DISCARDABLE_TYPES


def command_coalesce_key(command: dict) -> str:
    key = str(command.get("coalesce_key") or "").strip()
    if key:
        return key
    command_type = str(command.get("type") or "")
    if command_type in COMMAND_COALESCE_BY_TYPE:
        return command_type
    return ""


def command_contains_speech(command: dict) -> bool:
    command_type = str(command.get("type") or "")
    if command_type == "speak":
        return True
    payload = command.get("payload")
    if command_type == "sequence" and isinstance(payload, list):
        for step in payload:
            if isinstance(step, dict) and command_contains_speech(step):
                return True
    return False


def normalize_command_speech_payload(command_type: str, payload) -> None:
    if command_type == "speak" and isinstance(payload, dict):
        text = normalize_speech_text_for_voice(str(payload.get("text") or ""))
        payload["text"] = "" if speech_text_is_temporarily_suppressed(text) else text
        if text and not payload.get("cache_name"):
            cache_name = AUTO_SPEECH_CACHE_BY_TEXT.get(text)
            if cache_name:
                payload["cache_name"] = cache_name
        return
    if command_type == "sequence" and isinstance(payload, list):
        for step in payload:
            if isinstance(step, dict) and step.get("type") == "speak":
                text = normalize_speech_text_for_voice(str(step.get("text") or ""))
                step["text"] = "" if speech_text_is_temporarily_suppressed(text) else text
                if text and not step.get("cache_name"):
                    cache_name = AUTO_SPEECH_CACHE_BY_TEXT.get(text)
                    if cache_name:
                        step["cache_name"] = cache_name


def apply_current_speech_generation(server, device_id: str, command_type: str, payload) -> None:
    """Attach the generation expected by firmware to non-Morrow speech commands."""
    generation = 0
    coordinator = getattr(server, "morrow_coordinator", None)
    generation_getter = getattr(coordinator, "generation_for_device", None)
    if callable(generation_getter):
        generation = max(0, int(generation_getter(device_id)))
    else:
        command_store = getattr(server, "command_store", None)
        generation_getter = getattr(command_store, "speech_generation_for_device", None)
        if callable(generation_getter):
            generation = max(0, int(generation_getter(device_id)))

    if command_type == "speak" and isinstance(payload, dict):
        payload.setdefault("generation", generation)
        return
    if command_type == "sequence" and isinstance(payload, list):
        for step in payload:
            if not isinstance(step, dict) or step.get("type") != "speak":
                continue
            step_payload = step.get("payload")
            if isinstance(step_payload, dict):
                step_payload.setdefault("generation", generation)
            else:
                step.setdefault("generation", generation)


def clamp_speaker_volume(value) -> int:
    try:
        percent = int(value)
    except (TypeError, ValueError):
        percent = SPEAKER_VOLUME_DEFAULT
    return max(SPEAKER_VOLUME_MIN, min(SPEAKER_VOLUME_MAX, percent))


def apply_global_speaker_volume(command_type: str, payload, speaker_volume) -> None:
    """Attach the Server-owned hardware volume to every command that can speak."""
    percent = clamp_speaker_volume(speaker_volume)
    if command_type == "speak" and isinstance(payload, dict):
        payload["speaker_volume"] = percent
        return
    if command_type in ("find_owner", "locate_owner") and isinstance(payload, dict):
        payload["speaker_volume"] = percent
        return
    if command_type == "sequence" and isinstance(payload, list):
        for step in payload:
            if not isinstance(step, dict):
                continue
            step_type = str(step.get("type") or "")
            step_payload = step.get("payload")
            if not isinstance(step_payload, (dict, list)):
                step_payload = step
            apply_global_speaker_volume(step_type, step_payload, percent)


def normalize_server_volume_command(server, payload) -> int:
    """Resolve a relative volume command and update the Server source of truth."""
    if not isinstance(payload, dict):
        payload = {}
    current = clamp_speaker_volume(getattr(server, "speaker_volume", SPEAKER_VOLUME_DEFAULT))
    direction = str(payload.get("direction") or payload.get("action") or payload.get("type") or "").lower()
    mode = str(payload.get("mode") or "").lower()
    if mode == "set" or "value" in payload:
        target = clamp_speaker_volume(payload.get("value", current))
    else:
        try:
            step = abs(int(payload.get("step", 10))) or 10
        except (TypeError, ValueError):
            step = 10
        if direction in ("down", "lower", "small", "quiet"):
            target = clamp_speaker_volume(current - step)
        else:
            target = clamp_speaker_volume(current + step)
    server.speaker_volume = target
    payload["mode"] = "set"
    payload["value"] = target
    payload.pop("direction", None)
    payload.pop("action", None)
    payload.pop("step", None)
    return target


def prepare_server_command_audio(server, command_type: str, payload) -> None:
    """Apply volume changes in command order and stamp speech with the active global value."""
    if command_type in ("volume", "sound"):
        normalize_server_volume_command(server, payload)
        return
    if command_type == "sequence" and isinstance(payload, list):
        for step in payload:
            if not isinstance(step, dict):
                continue
            step_type = str(step.get("type") or "")
            step_payload = step.get("payload")
            if not isinstance(step_payload, (dict, list)):
                step_payload = step
            prepare_server_command_audio(server, step_type, step_payload)
        return
    apply_global_speaker_volume(
        command_type,
        payload,
        getattr(server, "speaker_volume", SPEAKER_VOLUME_DEFAULT),
    )


class DeviceCommandQueue:
    def __init__(self, max_size: int = COMMAND_QUEUE_MAX_SIZE):
        self.max_size = max(1, int(max_size))
        self._items = []
        self._seq = 0
        self._cv = threading.Condition()

    def qsize(self) -> int:
        with self._cv:
            self._drop_expired_locked(time.time())
            return len(self._items)

    def put(self, command: dict) -> dict:
        now = time.time()
        command_type = str(command.get("type") or "")
        priority = max(int(command.get("priority") or 0), command_default_priority(command_type))
        ttl = float(command.get("ttl_seconds") or command_default_ttl(command_type))
        expires_at = now + ttl if ttl > 0 else 0.0
        item = {
            "command": command,
            "priority": priority,
            "seq": self._seq,
            "expires_at": expires_at,
            "coalesce_key": command_coalesce_key(command),
            "discardable": command_is_discardable(command),
        }

        with self._cv:
            self._seq += 1
            stats = {"queued": False, "expired": self._drop_expired_locked(now), "preempted": 0, "coalesced": 0, "dropped": 0}

            if command.get("interrupt"):
                kept = []
                for existing in self._items:
                    if existing["priority"] <= priority or existing["discardable"]:
                        stats["preempted"] += 1
                    else:
                        kept.append(existing)
                self._items = kept

            if item["coalesce_key"]:
                kept = []
                for existing in self._items:
                    if existing["coalesce_key"] == item["coalesce_key"] and existing["priority"] <= priority:
                        stats["coalesced"] += 1
                    else:
                        kept.append(existing)
                self._items = kept

            while len(self._items) >= self.max_size:
                drop_index = self._find_drop_index_locked(priority)
                if drop_index is None:
                    stats["dropped"] += 1
                    return stats
                self._items.pop(drop_index)
                stats["dropped"] += 1

            self._items.append(item)
            stats["queued"] = True
            self._cv.notify()
            return stats

    def get(
        self,
        timeout: float | None = None,
        *,
        boot_id: int = 0,
        allow_speak: bool = True,
        allow_find_owner: bool | Callable[[], bool] = True,
    ) -> dict:
        deadline = None if timeout is None else time.monotonic() + timeout
        with self._cv:
            while True:
                self._drop_expired_locked(time.time())
                find_owner_allowed = allow_find_owner() if callable(allow_find_owner) else allow_find_owner
                eligible = [
                    index
                    for index, item in enumerate(self._items)
                    if (
                        int(boot_id or 0) <= 0
                        or int(item["command"].get("boot_id") or 0) in (0, int(boot_id))
                    )
                    and (allow_speak or str(item["command"].get("type") or "") != "speak")
                    and (
                        find_owner_allowed
                        or str(item["command"].get("type") or "") not in ("find_owner", "locate_owner")
                    )
                ]
                if eligible:
                    index = max(eligible, key=lambda i: (self._items[i]["priority"], -self._items[i]["seq"]))
                    return self._items.pop(index)["command"]
                if timeout == 0:
                    raise Empty
                if deadline is None:
                    self._cv.wait()
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise Empty
                self._cv.wait(remaining)

    def get_nowait(
        self,
        *,
        boot_id: int = 0,
        allow_speak: bool = True,
        allow_find_owner: bool | Callable[[], bool] = True,
    ) -> dict:
        return self.get(
            timeout=0,
            boot_id=boot_id,
            allow_speak=allow_speak,
            allow_find_owner=allow_find_owner,
        )

    def discard(self, cmd_id: str) -> bool:
        cmd_id = str(cmd_id or "")
        if not cmd_id:
            return False
        with self._cv:
            for index, item in enumerate(self._items):
                if str(item["command"].get("cmd_id") or "") == cmd_id:
                    self._items.pop(index)
                    return True
        return False

    def discard_other_boots(self, boot_id: int) -> int:
        """Remove commands that were bound to an older incarnation of this device."""
        boot_id = max(0, int(boot_id or 0))
        if boot_id <= 0:
            return 0
        with self._cv:
            kept = []
            discarded = 0
            for item in self._items:
                command_boot_id = max(0, int(item["command"].get("boot_id") or 0))
                if command_boot_id > 0 and command_boot_id != boot_id:
                    discarded += 1
                else:
                    kept.append(item)
            self._items = kept
            if discarded:
                self._cv.notify_all()
            return discarded

    def discard_speech_before_generation(self, generation: int) -> int:
        """Drop queued speech superseded by a device/session reset."""
        generation = max(0, int(generation))
        with self._cv:
            kept = []
            discarded = 0
            for item in self._items:
                command = item["command"]
                payload = command.get("payload")
                try:
                    command_generation = int(payload.get("generation") or 0) if isinstance(payload, dict) else 0
                except (TypeError, ValueError):
                    command_generation = 0
                if str(command.get("type") or "") == "speak" and command_generation < generation:
                    discarded += 1
                else:
                    kept.append(item)
            self._items = kept
            if discarded:
                self._cv.notify_all()
            return discarded

    def _drop_expired_locked(self, now: float) -> int:
        before = len(self._items)
        self._items = [
            item for item in self._items if item["expires_at"] <= 0 or item["expires_at"] > now
        ]
        return before - len(self._items)

    def _find_drop_index_locked(self, incoming_priority: int) -> int | None:
        discardable = [
            (item["priority"], item["seq"], index)
            for index, item in enumerate(self._items)
            if item["discardable"] and item["priority"] <= incoming_priority
        ]
        if discardable:
            return min(discardable)[2]
        lower_or_equal = [
            (item["priority"], item["seq"], index)
            for index, item in enumerate(self._items)
            if item["priority"] <= incoming_priority
        ]
        if lower_or_equal:
            return min(lower_or_equal)[2]
        return None


class AliyunVoiceServer(ThreadingHTTPServer):
    token: str
    token_expire_time: int
    access_key_id: str
    access_key_secret: str
    appkey: str
    asr_url: str
    tts_url: str
    voice: str
    asr_sample_rate: int
    sample_rate: int
    volume: int
    speaker_volume: int
    speech_rate: int
    pitch_rate: int
    max_sentence_chars: int
    chunk_size: int
    tts_prefetch_workers: int
    tts_request_timeout: int
    tts_retries: int
    tts_tail_silence_ms: int
    capture_save_mode: str
    save_audio_uploads: bool
    audio_capture_dir: str
    device_log_dir: str
    command_queue_max_size: int
    capture_dir: str
    static_dir: str
    morrow_client: MorrowClient | None
    morrow_coordinator: MorrowTurnCoordinator | None
    morrow_notice_stop_event: threading.Event
    morrow_notice_thread: threading.Thread | None
    morrow_web_gateway: MorrowWebGateway
    debug_log: bool
    device_lock: threading.Lock
    face_detector_backend: str
    face_detector: YunetFaceService | None
    visual_tracking_enabled: bool
    visual_tracking_deadzone_px: float
    visual_tracking_gain_x: float
    visual_tracking_gain_y: float
    visual_tracking_max_degree: float
    visual_tracking_min_degree: float
    visual_tracking_duration_ms: int
    visual_tracking_min_interval_ms: int
    visual_tracking_max_pending: int
    visual_tracking_invert_x: bool
    visual_tracking_invert_y: bool
    visual_tracking_last_command_at: dict[str, float]
    find_owner_gain_x: float
    find_owner_gain_y: float
    find_owner_stop_pixels: float
    realtime_manager: RealtimeManager | None
    realtime_ws_path: str
    realtime_ws_port: int
    realtime_public_host: str
    realtime_local_token: str
    ota_firmware_dir: str
    ota_firmware_file: str
    ota_public_base_url: str
    ota_force: bool
    ota_check_interval_seconds: int
    sedentary_reminder_interval_seconds: int
    sedentary_reminder_trigger_total: int
    sedentary_reminder_queued_total: int
    device_queues: dict[str, DeviceCommandQueue]
    last_ack: dict[str, dict]
    last_seen: dict[str, float]
    device_order: list[str]
    debug_config_lock: threading.Lock
    wifi_log_enabled: bool
    usb_serial_enabled: bool
    state_events_enabled: bool
    device_config_poll_ms: int
    device_log_post_interval_ms: int
    device_logs: dict[str, list[dict]]
    recording_cache: list[dict]
    v3_database: Database
    command_store: CommandStore
    device_registry: DeviceRegistry

    def get_token(self) -> str:
        if self.access_key_id and self.access_key_secret:
            now = int(time.time())
            if not self.token or now >= self.token_expire_time - TOKEN_REFRESH_MARGIN_SECONDS:
                self.token, self.token_expire_time = create_aliyun_nls_token(
                    self.access_key_id, self.access_key_secret
                )
                if getattr(self, "debug_log", False):
                    log_print(f"阿里云 NLS token 已刷新，expires_at={self.token_expire_time}")
                else:
                    log_print("阿里云 NLS token 已刷新")
        return self.token


class Handler(BaseHTTPRequestHandler):
    server_version = "XiaopaiAliyunVoice/1.0"

    def _debug_enabled(self) -> bool:
        return bool(getattr(self.server, "debug_log", False))

    def _log_info(self, message: str) -> None:
        log_print(message)

    def _log_debug(self, message: str) -> None:
        if self._debug_enabled():
            log_print(message)

    def _log_error(self, message: str) -> None:
        log_print(message, file=sys.stderr)

    def do_GET(self):
        path, query = self._path_query()
        if path in ("/web", "/web/"):
            self._handle_web_page()
            return
        if path == "/web/api/status":
            self._send_json(self.server.morrow_web_gateway.status())
            return
        if path.startswith("/web/api/sessions/"):
            session_id = urllib.parse.unquote(path[len("/web/api/sessions/") :])
            if not session_id or "/" in session_id:
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            try:
                session = self.server.morrow_web_gateway.get_session(session_id)
                self._send_json({"session_id": session_id, "session": session})
            except MorrowWebError as exc:
                self._send_json({"type": "error", "message": str(exc)}, exc.status)
            return
        if path in ("/", "/health"):
            self._send_json(
                {
                    "ok": True,
                    "service": "xiaopai-aliyun-voice",
                    "protocol_version": PROTOCOL_VERSION,
                    "v3": {
                        "tts": "/v3/tts",
                        "ota_manifest": "/v3/ota/manifest",
                        "control_hello": "/v3/control/hello",
                    },
                    "asr": "/upload",
                    "tts": "/stream-speak?text=...",
                    "tts_debug": f"/tts/debug?text=...&voice={DEFAULT_TTS_VOICE}&format=wav",
                    "tts_voices": "/tts/voices",
                    "image": "/upload-image",
                    "tts_format": "pcm_s16le",
                    "sample_rate": self.server.sample_rate,
                    "asr_sample_rate": self.server.asr_sample_rate,
                    "tts_sample_rate": self.server.sample_rate,
                    "channels": 1,
                    "voice": self.server.voice,
                    "speaker_volume": self.server.speaker_volume,
                    "expressions": list(AVAILABLE_EXPRESSIONS),
                    "actions": list(AVAILABLE_ACTIONS),
                    "head_touch_events": HEAD_TOUCH_EVENT_TEXT,
                    "face_detector": self.server.face_detector.status()
                    if self.server.face_detector is not None
                    else {"backend": self.server.face_detector_backend, "available": False},
                    "visual_tracking": {
                        "enabled": self.server.visual_tracking_enabled,
                        "deadzone_px": self.server.visual_tracking_deadzone_px,
                        "gain_x": self.server.visual_tracking_gain_x,
                        "gain_y": self.server.visual_tracking_gain_y,
                        "max_degree": self.server.visual_tracking_max_degree,
                        "min_degree": self.server.visual_tracking_min_degree,
                        "duration_ms": self.server.visual_tracking_duration_ms,
                        "min_interval_ms": self.server.visual_tracking_min_interval_ms,
                        "max_pending": self.server.visual_tracking_max_pending,
                        "invert_x": self.server.visual_tracking_invert_x,
                        "invert_y": self.server.visual_tracking_invert_y,
                    },
                    "find_owner": {
                        "gain_x": self.server.find_owner_gain_x,
                        "gain_y": self.server.find_owner_gain_y,
                        "stop_pixels": self.server.find_owner_stop_pixels,
                    },
                    "sedentary_reminder": {
                        "enabled": self.server.sedentary_reminder_interval_seconds > 0,
                        "interval_seconds": self.server.sedentary_reminder_interval_seconds,
                        "trigger_total": self.server.sedentary_reminder_trigger_total,
                        "queued_total": self.server.sedentary_reminder_queued_total,
                    },
                    "morrow": self._morrow_health(),
                    "web_chat": "/web",
                    "commands": self._command_health(),
                    "devices": self._device_health(),
                    "realtime": self._realtime_status(),
                    "ota": self._ota_status(),
                    "debug_config": self._debug_config_body(),
                    "command_queue": {
                        "max_size": self.server.command_queue_max_size,
                        "default_priorities": COMMAND_DEFAULT_PRIORITIES,
                        "coalesced_types": sorted(COMMAND_COALESCE_BY_TYPE),
                        "discardable_types": sorted(COMMAND_DISCARDABLE_TYPES),
                    },
                }
            )
            return
        if path == "/metrics":
            self._handle_metrics()
            return
        if path == "/v3/devices":
            self._handle_v3_devices()
            return
        if path == "/v3/ota/manifest":
            self._handle_v3_ota_manifest()
            return
        if path.startswith("/v3/ota/images/"):
            self._handle_firmware_download(path.rsplit("/", 1)[-1])
            return
        if path == "/v3/device/next-command":
            self._handle_next_command(query)
            return
        if path in ("/ota", "/ota/"):
            self._handle_ota(query)
            return
        if path.startswith("/firmware/"):
            if path in ("/firmware/latest.json", "/firmware/latest"):
                self._handle_firmware_latest()
                return
            self._handle_firmware_download(path.rsplit("/", 1)[-1])
            return
        if path == "/expressions":
            self._send_json(
                {
                    "type": "expressions",
                    "expressions": list(AVAILABLE_EXPRESSIONS),
                    "actions": list(AVAILABLE_ACTIONS),
                    "aliases": EXPRESSION_ALIASES,
                    "examples": {
                        "expression": "/expression/shy?device_id=...",
                        "action": "/action/blink?device_id=...",
                    },
                }
            )
            return
        if path.startswith("/expression/"):
            expression = urllib.parse.unquote(path.rsplit("/", 1)[-1])
            self._handle_face_shortcut(query, expression)
            return
        if path.startswith("/action/"):
            action = urllib.parse.unquote(path.rsplit("/", 1)[-1])
            self._handle_face_shortcut(query, action, action_only=True)
            return
        if path == "/devices":
            self._handle_devices()
            return
        if path == "/device/config":
            self._handle_device_config(query)
            return
        if path == "/debug/config":
            self._handle_debug_config(query)
            return
        if path == "/device/logs":
            self._handle_device_logs(query)
            return
        if path == "/debug/recordings":
            self._handle_recordings(query)
            return
        if path == "/command":
            self._handle_command(query)
            return
        if path.startswith("/command/"):
            command_type = path.rsplit("/", 1)[-1]
            self._handle_command(query, command_type=command_type)
            return
        if path == "/device/next-command":
            self._handle_next_command(query)
            return
        if path in ("/device/event", "/event"):
            self._handle_device_event(query)
            return
        if path == "/head-touch-events":
            self._send_json(
                {
                    "type": "head_touch_events",
                    "events": [
                        {
                            "name": name,
                            "text": text,
                            "audio": f"/event-audio/{name}.pcm",
                            "wav": f"/event-audio/{name}.wav",
                        }
                        for name, text in HEAD_TOUCH_EVENT_TEXT.items()
                    ],
                    "format": "pcm_s16le",
                    "sample_rate": self.server.sample_rate,
                    "channels": 1,
                }
            )
            return
        if path == "/tts/voices":
            self._handle_tts_voices()
            return
        if path == "/tts/debug":
            self._handle_tts_debug(query)
            return
        if path.startswith("/event-audio/"):
            self._handle_event_audio(path.rsplit("/", 1)[-1])
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        path, query = self._path_query()
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        if path == "/web/api/morrow/mode":
            try:
                payload = json.loads(body.decode("utf-8")) if body else {}
                if not isinstance(payload, dict):
                    raise MorrowWebError("request body must be a JSON object", 400)
                result = self.server.morrow_web_gateway.switch_mode(payload.get("mode", ""))
                self._send_json(result)
            except (json.JSONDecodeError, UnicodeDecodeError):
                self._send_json({"type": "error", "message": "invalid JSON body"}, HTTPStatus.BAD_REQUEST)
            except MorrowWebError as exc:
                self._send_json({"type": "error", "message": str(exc)}, exc.status)
            return
        if path == "/web/api/sessions":
            try:
                self._send_json(self.server.morrow_web_gateway.create_session(), HTTPStatus.CREATED)
            except MorrowWebError as exc:
                self._send_json({"type": "error", "message": str(exc)}, exc.status)
            return
        if path.startswith("/web/api/sessions/") and path.endswith("/messages"):
            session_path = path[len("/web/api/sessions/") : -len("/messages")]
            session_id = urllib.parse.unquote(session_path.rstrip("/"))
            try:
                payload = json.loads(body.decode("utf-8")) if body else {}
                if not isinstance(payload, dict):
                    raise MorrowWebError("request body must be a JSON object", 400)
                result = self.server.morrow_web_gateway.send_message(session_id, payload.get("message", ""))
                self._send_json(result)
            except (json.JSONDecodeError, UnicodeDecodeError):
                self._send_json({"type": "error", "message": "invalid JSON body"}, HTTPStatus.BAD_REQUEST)
            except MorrowWebError as exc:
                self._send_json({"type": "error", "message": str(exc)}, exc.status)
            return
        if path == "/upload":
            self._handle_upload(body)
            return
        if path == "/v3/control/hello":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_v3_hello(payload)
            return
        if path == "/v3/control/heartbeat":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_v3_heartbeat(payload)
            return
        if path == "/v3/command_ack":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_v3_command_ack(payload)
            return
        if path == "/v3/tts":
            self._handle_stream_speak(query, body)
            return
        if path == "/v3/vision/captures":
            self._handle_upload_image(body)
            return
        if path == "/upload-audio":
            self._handle_upload(body)
            return
        if path in ("/ota", "/ota/"):
            self._handle_ota(query)
            return
        if path == "/command":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_command(query, posted=payload)
            return
        if path == "/debug/config":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_debug_config(query, posted=payload)
            return
        if path == "/device/logs":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_device_logs(query, posted=payload)
            return
        if path in ("/device/event", "/event"):
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_device_event(query, posted=payload)
            return
        if path == "/upload-image":
            self._handle_upload_image(body)
            return
        if path == "/tts/debug":
            self._handle_tts_debug(query, body)
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def _path_query(self):
        parsed = urllib.parse.urlparse(self.path)
        return parsed.path, urllib.parse.parse_qs(parsed.query)

    def _handle_web_page(self) -> None:
        page_path = os.path.join(self.server.static_dir, "web", "index.html")
        try:
            with open(page_path, "rb") as handle:
                content = handle.read()
        except OSError as exc:
            self._log_error(f"网页加载失败: path={page_path} error={exc}")
            self.send_error(HTTPStatus.INTERNAL_SERVER_ERROR, "web chat page is unavailable")
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
            "connect-src 'self'; img-src 'self' data:",
        )
        self.end_headers()
        self.wfile.write(content)

    def _handle_v3_devices(self) -> None:
        now = time.time()
        registry_devices = {item["device_id"]: item for item in self.server.device_registry.list_devices()}
        with self.server.device_lock:
            known_device_ids = list(self.server.device_order)
            last_seen_snapshot = dict(self.server.last_seen)
            last_ack_snapshot = dict(self.server.last_ack)
        for device_id, seen in last_seen_snapshot.items():
            item = registry_devices.setdefault(device_id, {"device_id": device_id, "capabilities": []})
            item["last_seen_seconds_ago"] = round(now - seen, 1)
            item["online"] = now - seen <= DEVICE_ONLINE_TTL_SECONDS
            item["pending_commands"] = self._queue_for(device_id).qsize()
            item["last_ack"] = last_ack_snapshot.get(device_id)
        ordered = []
        for device_id in known_device_ids:
            if device_id in registry_devices:
                ordered.append(registry_devices.pop(device_id))
        ordered.extend(registry_devices.values())
        self._send_json(
            {
                "type": "devices",
                "protocol_version": PROTOCOL_VERSION,
                "default_device_id": first_connected_device_id(last_seen_snapshot, known_device_ids),
                "online_ttl_seconds": DEVICE_ONLINE_TTL_SECONDS,
                "devices": ordered,
            }
        )

    def _handle_v3_hello(self, payload: dict) -> None:
        body = self.server.device_registry.hello(payload)
        device_id = safe_device_id(payload.get("device_id") or "default")
        self._observe_device_boot(device_id, payload.get("boot_id"), authoritative=True)
        self._sync_device_speech_generation(device_id, payload)
        self._mark_device_seen(device_id)
        self._send_json(body)

    def _observe_device_boot(self, device_id: str, boot_id, *, authoritative: bool = False) -> bool:
        try:
            boot_id = max(0, int(boot_id or 0))
        except (TypeError, ValueError):
            return False
        if boot_id <= 0:
            return False
        expired = 0
        command_store = getattr(self.server, "command_store", None)
        if command_store is not None:
            current_boot_id = command_store.current_boot_id(device_id)
            if current_boot_id > 0 and current_boot_id != boot_id and not authoritative:
                command_store.expire_inactive_boot_commands()
                self._log_info(
                    f"已忽略过期 boot 请求: device={device_id} "
                    f"boot={boot_id} current_boot={current_boot_id}"
                )
                return False
            if current_boot_id != boot_id or authoritative:
                expired = command_store.observe_device_boot(device_id, boot_id)
        queue = self._queue_for(device_id)
        discarded = queue.discard_other_boots(boot_id)
        if expired or discarded:
            self._log_info(
                f"设备 boot 切换已清理旧命令: device={device_id} boot={boot_id} "
                f"expired={expired} memory_discarded={discarded}"
            )
        return True

    def _sync_device_speech_generation(self, device_id: str, payload: dict) -> None:
        if "speech_generation" not in payload:
            return
        try:
            generation = max(0, int(payload.get("speech_generation") or 0))
        except (TypeError, ValueError):
            return
        coordinator = getattr(self.server, "morrow_coordinator", None)
        if coordinator is not None:
            coordinator.sync_device_generation(device_id, generation)
            return
        command_store = getattr(self.server, "command_store", None)
        if command_store is not None:
            command_store.set_speech_generation(device_id, generation)

    def _handle_v3_heartbeat(self, payload: dict) -> None:
        device_id = safe_device_id(payload.get("device_id") or self.headers.get("X-Device-Id", "") or "default")
        boot_id = max(0, int(payload.get("boot_id") or 0))
        self._observe_device_boot(device_id, boot_id)
        body = self.server.device_registry.heartbeat(
            device_id,
            boot_id=boot_id,
            last_ack_seq=int(payload.get("last_ack_seq") or 0),
        )
        self._sync_device_speech_generation(device_id, payload)
        self._mark_device_seen(device_id)
        self._log_info(
            f"设备心跳状态: device={device_id} boot={payload.get('boot_id')} "
            f"mode={payload.get('mode')} face={payload.get('expression')} "
            f"audio={payload.get('audio_input')} pending={payload.get('audio_input_pending')} "
            f"dji_detected={payload.get('dji_detected')} dji_id={payload.get('dji_identity_confirmed')} "
            f"dji_ready={payload.get('dji_capture_ready')} usb_output={payload.get('usb_output')} "
            f"vbus_mv={payload.get('vbus_mv')} reset={payload.get('reset_reason')} "
            f"logs={payload.get('network_log_uploaded')}/{payload.get('network_log_captured')} "
            f"dropped={payload.get('network_log_dropped')} "
            f"heap_internal={payload.get('free_internal_heap')} "
            f"heap_psram={payload.get('free_psram')} "
            f"queue_depth={payload.get('speech_queue_depth')}"
        )
        self._send_json(body)

    def _handle_v3_command_ack(self, payload: dict) -> None:
        if not isinstance(payload, dict):
            self._send_json({"type": "error", "message": "ack body must be a JSON object"}, HTTPStatus.BAD_REQUEST)
            return
        result = self.server.command_store.record_ack(payload)
        device_id = safe_device_id(payload.get("device_id") or self.headers.get("X-Device-Id", "") or "default")
        with self.server.device_lock:
            self.server.last_ack[device_id] = {
                "cmd_id": result.get("cmd_id", ""),
                "status": result.get("state", ""),
                "message": payload.get("message", ""),
                "ts": time.time(),
            }
        self._mark_device_seen(device_id)
        self._send_json({"type": "command_ack", "device_id": device_id, **result})

    def _handle_v3_ota_manifest(self) -> None:
        firmware = find_latest_ota_firmware(self.server.ota_firmware_file, self.server.ota_firmware_dir)
        if firmware is None:
            self.send_error(HTTPStatus.NOT_FOUND, "OTA firmware is not configured")
            return
        body = ota_firmware_manifest(self.server, self.headers, firmware)
        body["image_url"] = body.pop("url")
        body["signature_required"] = False
        body["rollback_supported"] = True
        self._send_json({"type": "ota_manifest", "firmware": body})

    def _realtime_status(self) -> dict:
        manager = getattr(self.server, "realtime_manager", None)
        return {
            "enabled": bool(manager and manager.enabled),
            "ws_path": getattr(self.server, "realtime_ws_path", "/ws"),
            "ws_port": getattr(self.server, "realtime_ws_port", 0),
            "devices": len(manager.devices_snapshot()) if manager else 0,
            "upstream_sample_rate": manager.config.upstream_sample_rate if manager else self.server.asr_sample_rate,
            "downstream_sample_rate": manager.config.downstream_sample_rate if manager else self.server.sample_rate,
        }

    def _ota_status(self) -> dict:
        firmware = find_latest_ota_firmware(self.server.ota_firmware_file, self.server.ota_firmware_dir)
        body = {
            "enabled": firmware is not None,
            "firmware_dir": self.server.ota_firmware_dir,
            "firmware_file": self.server.ota_firmware_file,
            "public_base_url": self.server.ota_public_base_url,
            "force": self.server.ota_force,
        }
        if firmware is not None:
            body["firmware"] = {
                "path": firmware.path,
                "version": firmware.version,
                "project_name": firmware.project_name,
                "size": firmware.size,
                "sha256": firmware.sha256,
                "updated_at": _dt.datetime.fromtimestamp(firmware.mtime).isoformat(timespec="seconds"),
            }
        return body

    def _debug_config_body(self, device_id: str = "") -> dict:
        with self.server.debug_config_lock:
            save_recording = bool(getattr(self.server, "save_audio_uploads", True))
            return {
                "type": "device_config",
                "device_id": safe_device_id(device_id) if device_id else "",
                "wifi_log": bool(getattr(self.server, "wifi_log_enabled", True)),
                "usb_serial": bool(getattr(self.server, "usb_serial_enabled", True)),
                "state_events": bool(getattr(self.server, "state_events_enabled", True)),
                "save_recording": save_recording,
                "save-recording": save_recording,
                "save_audio_uploads": save_recording,
                "device_log_dir": str(getattr(self.server, "device_log_dir", "")),
                "config_poll_ms": int(getattr(self.server, "device_config_poll_ms", 5000)),
                "log_post_interval_ms": int(getattr(self.server, "device_log_post_interval_ms", 1000)),
            }

    def _optional_bool_value(self, values: dict, *keys: str) -> bool | None:
        for key in keys:
            if key not in values:
                continue
            value = values[key]
            if value is None:
                continue
            if isinstance(value, bool):
                return value
            if isinstance(value, (int, float)):
                return value != 0
            return parse_bool(str(value))
        return None

    def _apply_debug_config(self, values: dict) -> list[str]:
        changed = []
        wifi_log = self._optional_bool_value(values, "wifi_log", "wifi_logs", "wifi-log", "wifi-logs")
        usb_serial = self._optional_bool_value(values, "usb_serial", "usb-serial", "serial", "console")
        state_events = self._optional_bool_value(values, "state_events", "state-events", "state_log", "state-log")
        save_recording = self._optional_bool_value(
            values,
            "save_recording",
            "save-recording",
            "save_audio_uploads",
            "save-audio-uploads",
        )
        with self.server.debug_config_lock:
            if wifi_log is not None and wifi_log != self.server.wifi_log_enabled:
                self.server.wifi_log_enabled = wifi_log
                changed.append("wifi_log")
            if usb_serial is not None and usb_serial != self.server.usb_serial_enabled:
                self.server.usb_serial_enabled = usb_serial
                changed.append("usb_serial")
            if state_events is not None and state_events != self.server.state_events_enabled:
                self.server.state_events_enabled = state_events
                changed.append("state_events")
            if save_recording is not None and save_recording != self.server.save_audio_uploads:
                self.server.save_audio_uploads = save_recording
                manager = getattr(self.server, "realtime_manager", None)
                if manager is not None:
                    manager.config.save_audio_uploads = save_recording
                changed.append("save_recording")
        return changed

    def _handle_device_config(self, query: dict) -> None:
        device_id = self._device_id(query)
        self._mark_device_seen(device_id)
        self._send_json(self._debug_config_body(device_id))

    def _handle_debug_config(self, query: dict, posted: dict | None = None) -> None:
        values = {key: first_value(query, key) for key in query}
        if posted:
            values.update(posted)
        changed = self._apply_debug_config(values)
        body = self._debug_config_body(first_value(query, "device_id") or str(values.get("device_id") or ""))
        body["changed"] = changed
        self._send_json(body)

    def _append_device_log(self, device_id: str, event: dict) -> None:
        device_id = safe_device_id(device_id)
        event = dict(event)
        event["device_id"] = device_id
        event.setdefault("server_ts", time.time())
        with self.server.device_lock:
            logs = self.server.device_logs.setdefault(device_id, [])
            logs.append(event)
            if len(logs) > DEVICE_LOG_MAX_ITEMS:
                del logs[: len(logs) - DEVICE_LOG_MAX_ITEMS]
            self._append_device_log_file(device_id, event)

    def _append_device_log_file(self, device_id: str, event: dict) -> None:
        try:
            append_device_log_file(self.server, device_id, event)
        except OSError as exc:
            self._log_error(f"设备日志文件写入失败: device={safe_device_id(device_id)} error={exc}")

    def _handle_device_logs(self, query: dict, posted: dict | None = None) -> None:
        if posted is None:
            requested = self._device_id(query)
            limit = max(1, min(DEVICE_LOG_MAX_ITEMS, int(first_value(query, "limit") or "100")))
            with self.server.device_lock:
                if first_value(query, "device_id"):
                    logs = list(self.server.device_logs.get(requested, []))[-limit:]
                    body = {"type": "device_logs", "device_id": requested, "logs": logs}
                else:
                    body = {
                        "type": "device_logs",
                        "devices": {
                            device_id: list(logs)[-limit:]
                            for device_id, logs in self.server.device_logs.items()
                        },
                    }
            self._send_json(body)
            return

        device_id = safe_device_id(posted.get("device_id") or self._device_id(query))
        self._mark_device_seen(device_id)
        raw_events = posted.get("events")
        if isinstance(raw_events, list):
            events = raw_events
        elif isinstance(posted.get("event"), dict):
            events = [posted["event"]]
        else:
            events = [posted]

        accepted = 0
        for raw_event in events[:50]:
            if not isinstance(raw_event, dict):
                continue
            event = {
                key: value
                for key, value in raw_event.items()
                if isinstance(key, str) and key not in ("device_id",)
            }
            event_type = str(event.get("type") or event.get("event_type") or "log")
            event["type"] = event_type
            if "message" in event:
                event["message"] = truncate_log_text(str(event.get("message") or ""))
            if "line" in event:
                event["line"] = truncate_log_text(str(event.get("line") or ""))
            self._append_device_log(device_id, event)
            accepted += 1
            if event_type in ("state", "state_change"):
                source = event.get("source") or event.get("state_machine") or "state"
                old = event.get("from") or event.get("old") or ""
                new = event.get("to") or event.get("new") or event.get("state") or ""
                reason = event.get("reason") or ""
                self._log_info(f"设备状态事件: device={device_id} source={source} state={old}->{new} reason={reason}")
        self._send_json({"type": "device_logs_ack", "device_id": device_id, "accepted": accepted})

    def _append_recording_metadata(self, metadata: dict) -> None:
        item = dict(metadata)
        item.setdefault("ts", time.time())
        with self.server.device_lock:
            self.server.recording_cache.append(item)
            if len(self.server.recording_cache) > DEVICE_RECORDING_MAX_ITEMS:
                del self.server.recording_cache[: len(self.server.recording_cache) - DEVICE_RECORDING_MAX_ITEMS]

    def _handle_recordings(self, query: dict) -> None:
        limit = max(1, min(DEVICE_RECORDING_MAX_ITEMS, int(first_value(query, "limit") or "50")))
        device_filter = safe_device_id(first_value(query, "device_id")) if first_value(query, "device_id") else ""
        with self.server.device_lock:
            recordings = list(self.server.recording_cache)
        if device_filter:
            recordings = [item for item in recordings if safe_device_id(item.get("device_id", "")) == device_filter]
        self._send_json(
            {
                "type": "recordings",
                "save_recording": bool(getattr(self.server, "save_audio_uploads", True)),
                "recordings": recordings[-limit:],
            }
        )

    def _handle_ota(self, query: dict) -> None:
        host = first_value(query, "host") or getattr(self.server, "realtime_public_host", "")
        if not host:
            host_header = self.headers.get("Host", "")
            host = host_header.split(":", 1)[0] if host_header else "127.0.0.1"
        port = int(first_value(query, "ws_port") or getattr(self.server, "realtime_ws_port", 0) or self.server.server_port)
        path = getattr(self.server, "realtime_ws_path", "/ws")
        token = getattr(self.server, "realtime_local_token", "")
        ws_url = f"ws://{host}:{port}{path}"
        self._log_info(f"实时语音配置: host_header={self.headers.get('Host', '')!r} ws_url={ws_url}")
        body = ota_config(ws_url, token)
        firmware = find_latest_ota_firmware(self.server.ota_firmware_file, self.server.ota_firmware_dir)
        if firmware is not None:
            firmware_body = ota_firmware_manifest(self.server, self.headers, firmware)
            if self.server.ota_force:
                firmware_body["force"] = 1
            body["firmware"] = firmware_body
            self._log_info(
                f"OTA 固件已下发: version={firmware.version} "
                f"file={os.path.basename(firmware.path)} size={firmware.size}"
            )
        self._send_json(body)

    def _handle_firmware_latest(self) -> None:
        firmware = find_latest_ota_firmware(self.server.ota_firmware_file, self.server.ota_firmware_dir)
        if firmware is None:
            self.send_error(HTTPStatus.NOT_FOUND, "OTA firmware is not configured")
            return
        body = ota_firmware_manifest(self.server, self.headers, firmware)
        if self.server.ota_force:
            body["force"] = 1
        self._send_json(body)

    def _handle_firmware_download(self, raw_name: str) -> None:
        requested_name = urllib.parse.unquote(raw_name)
        firmware = find_latest_ota_firmware(self.server.ota_firmware_file, self.server.ota_firmware_dir)
        if firmware is None:
            self.send_error(HTTPStatus.NOT_FOUND, "OTA firmware is not configured")
            return
        if requested_name != os.path.basename(firmware.path):
            self.send_error(HTTPStatus.NOT_FOUND, "unknown firmware")
            return
        try:
            stat = os.stat(firmware.path)
            fp = open(firmware.path, "rb")
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "firmware unavailable")
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(stat.st_size))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with fp:
            while True:
                chunk = fp.read(1024 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)
        self._log_info(f"OTA 固件已发送: file={os.path.basename(firmware.path)} bytes={stat.st_size}")

    def _handle_upload(self, body: bytes):
        if not body:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing audio body")
            return

        path, query = self._path_query()
        device_id = self._device_id(query)
        self._mark_device_seen(device_id)
        sample_rate = detect_wav_sample_rate(body) or self.server.asr_sample_rate
        audio_format = "wav" if detect_wav_sample_rate(body) else "pcm"
        upload_mode = self.headers.get("X-Upload-Mode", "").strip().lower()
        save_only = (
            parse_bool(first_value(query, "save_only"))
            or parse_bool(first_value(query, "save-only"))
            or parse_bool(self.headers.get("X-Save-Only", ""))
            or upload_mode in ("save", "save-only", "save_only", "record", "record-only", "record_only")
        )
        save_raw = parse_bool(first_value(query, "save_raw")) or parse_bool(self.headers.get("X-Save-Raw", ""))
        upload_prefix = self.headers.get("X-Audio-Test-Name", "").strip() or first_value(query, "name") or "upload"
        self._log_info(f"音频上传已收到: format={audio_format} bytes={len(body)} save_only={save_only}")
        self._log_debug(f"音频上传详情: device={device_id} bytes={len(body)} format={audio_format} sample_rate={sample_rate}")
        audio_path = ""
        raw_path = ""
        save_error = ""
        if getattr(self.server, "save_audio_uploads", True):
            try:
                audio_path = save_audio_capture(
                    body,
                    capture_dir=self.server.audio_capture_dir,
                    device_id=device_id,
                    prefix=upload_prefix,
                    audio_format=audio_format,
                    sample_rate=sample_rate,
                )
                if audio_path and save_raw:
                    raw_path = save_wav_raw_sidecar(audio_path, body)
                if audio_path:
                    self._log_info(f"ASR 上传音频已保存: device={device_id} path={audio_path}")
                    if raw_path:
                        self._log_info(f"ASR 上传原始 PCM 已保存: device={device_id} path={raw_path}")
                    self._append_recording_metadata(
                        {
                            "source": "http-upload",
                            "device_id": device_id,
                            "path": audio_path,
                            "raw_path": raw_path,
                            "bytes": len(body),
                            "audio_format": audio_format,
                            "sample_rate": sample_rate,
                        }
                    )
            except Exception as exc:
                save_error = str(exc)
                self._log_error(f"ASR 上传音频保存失败: {exc}")
        if save_only:
            if not audio_path:
                message = save_error or "audio saving is disabled"
                self._send_json({"type": "error", "message": message, "device_id": device_id}, HTTPStatus.INTERNAL_SERVER_ERROR)
                return
            self._log_info(f"音频保存模式完成: device={device_id} path={audio_path or '-'}")
            self._send_json(
                {
                    "type": "audio_saved",
                    "device_id": device_id,
                    "path": audio_path,
                    "raw_path": raw_path,
                    "bytes": len(body),
                    "audio_format": audio_format,
                    "sample_rate": sample_rate,
                    "saved": bool(audio_path),
                }
            )
            return
        try:
            result = self._aliyun_asr(body, audio_format, sample_rate)
        except Exception as exc:
            self._log_error(f"ASR 识别失败: {exc}")
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        text = result.get("result", "")
        status = result.get("status")
        message = result.get("message", "")
        if text:
            self._log_info(f"ASR 识别结果: {text!r}")
        else:
            self._log_info("ASR 未识别到语音")
        self._log_debug(
            "ASR 结果详情: "
            f"device={device_id} status={status} task_id={result.get('task_id', '')!r} "
            f"text={text!r} message={message!r}"
        )
        if status != 20000000:
            self._send_json({"type": "error", "message": message or f"Aliyun ASR status {status}"}, HTTPStatus.BAD_GATEWAY)
            return

        response = {"type": "stt", "text": text, "task_id": result.get("task_id", ""), "device_id": device_id}
        if text:
            volume_command = parse_voice_volume_command(text)
            if volume_command is not None:
                command = make_command("volume", volume_command, priority=1, interrupt=True)
                self._enqueue_command(device_id, command)
                response["handled_as"] = "volume_adjust"
                response["dialog_awake"] = self._dialog_awake(device_id)
                response["queued_command"] = command["cmd_id"]
                response["volume_direction"] = volume_command.get("direction") or volume_command.get("mode") or "set"
                self._send_json(response)
                return

            if has_dialog_sleep_word(text):
                sleep_reply_name, sleep_reply_text = sleep_reply_event_for_text(text)
                sleep_reply_command = make_command(
                    "speak",
                    {"text": sleep_reply_text, "cache_name": sleep_reply_name, "pause_listener": True},
                    priority=95,
                    interrupt=True,
                    ttl_seconds=8,
                    discardable=False,
                    coalesce_key="sleep_reply",
                )
                self._enqueue_command(device_id, sleep_reply_command)
                self._sleep_dialog(device_id, reason=text)
                response["handled_as"] = "sleep"
                response["dialog_awake"] = False
                response["queued_command"] = sleep_reply_command["cmd_id"]
                response["sleep_reply"] = {"name": sleep_reply_name, "text": sleep_reply_text}
                self._send_json(response)
                return

            woke_by_word = has_dialog_wake_word(text)
            if woke_by_word:
                self._wake_dialog(device_id, reason=text)
                response["woke_by"] = "wake_word"
                wake_reply_name, wake_reply_text = random.choice(WAKE_REPLY_EVENTS)
                wake_reply_command = make_command(
                    "speak",
                    {"text": wake_reply_text, "cache_name": wake_reply_name, "pause_listener": True},
                    priority=95,
                    interrupt=True,
                    ttl_seconds=8,
                    discardable=False,
                    coalesce_key="wake_reply",
                )
                self._enqueue_command(device_id, wake_reply_command)
                response["queued_command"] = wake_reply_command["cmd_id"]
                response["queued_commands"] = [wake_reply_command["cmd_id"]]
                response["wake_reply"] = {"name": wake_reply_name, "text": wake_reply_text}
                if is_wake_only_text(text):
                    response["handled_as"] = "wake"
                    response["dialog_awake"] = True
                    self._send_json(response)
                    return
            elif not self._dialog_awake(device_id):
                response["handled_as"] = "sleeping"
                response["dialog_awake"] = False
                self._log_info("ASR 在休眠状态下已忽略")
                self._log_debug(f"ASR 休眠详情: device={device_id} text={text!r}")
                self._send_json(response)
                return
            else:
                self._wake_dialog(device_id, reason="dialog activity")

            response["dialog_awake"] = True
            morrow_result = self._send_morrow_event(
                device_id,
                "speech_recognition",
                {"text": text, "task_id": result.get("task_id", "")},
            )
            response.update(morrow_result)
            response["handled_as"] = "morrow_forwarded" if morrow_result.get("morrow_submitted") else "morrow_not_sent"
        else:
            response["handled_as"] = "empty"
            self._log_info("ASR 为空，已跳过 Morrow")
            self._log_debug(f"ASR 空结果详情: device={device_id}")
        self._send_json(response)

    def _handle_device_event(self, query: dict, posted: dict | None = None):
        posted = posted or {}
        device_id = self._device_id(query) if query else posted.get("device_id", "default")
        device_id = safe_device_id(posted.get("device_id") or device_id)
        self._mark_device_seen(device_id)

        event_type = first_value(query, "type") or posted.get("type") or posted.get("event_type") or "event"
        name = first_value(query, "name") or posted.get("name") or posted.get("event") or ""
        text = first_value(query, "text") or posted.get("text") or ""
        details = posted.get("details") if isinstance(posted.get("details"), dict) else {}
        if not details:
            details = {key: values[0] for key, values in query.items() if values and key not in ("device_id",)}
        if name:
            details["name"] = name
        if text:
            details["text"] = text

        if str(event_type) in ("local_stop", "stop"):
            coordinator = getattr(self.server, "morrow_coordinator", None)
            generation = coordinator.cancel_device(device_id) if coordinator is not None else 0
            self._send_json(
                {
                    "type": "event",
                    "device_id": device_id,
                    "event_type": event_type,
                    "morrow_cancelled": coordinator is not None,
                    "generation": generation,
                    "queued_commands": [],
                }
            )
            return

        if str(event_type) in ("reset_session", "new_session"):
            coordinator = getattr(self.server, "morrow_coordinator", None)
            if coordinator is None:
                self._send_json(
                    {
                        "type": "event",
                        "device_id": device_id,
                        "event_type": event_type,
                        "session_reset": False,
                        "error": "Morrow is not configured",
                        "queued_commands": [],
                    },
                    HTTPStatus.SERVICE_UNAVAILABLE,
                )
                return

            reported_generation = posted.get("generation") or details.get("generation")
            if reported_generation not in (None, ""):
                try:
                    coordinator.sync_device_generation(device_id, max(0, int(reported_generation)))
                except (AttributeError, TypeError, ValueError):
                    pass
            result = coordinator.reset_session(device_id)
            queued_commands = []
            if result.success:
                command = make_command(
                    "speak",
                    {
                        "text": "你好，我是小派，今天有什么需要帮忙的？",
                        "generation": result.generation,
                    },
                    interrupt=False,
                    discardable=False,
                    coalesce_key="session_reset_reply",
                )
                if self._enqueue_command(device_id, command):
                    queued_commands.append(command["cmd_id"])
                self._log_info(f"Morrow 会话已由屏幕长按重置: device={device_id}")
            else:
                self._log_error(f"Morrow 会话重置失败: device={device_id} error={result.message}")
            self._send_json(
                {
                    "type": "event",
                    "device_id": device_id,
                    "event_type": event_type,
                    "session_reset": result.success,
                    "generation": result.generation,
                    "error": result.message,
                    "queued_commands": queued_commands,
                },
                HTTPStatus.OK if result.success else HTTPStatus.CONFLICT,
            )
            return

        if str(event_type) in ("head_touch", "touch"):
            command = make_command("face", {"expression": "shy"}, priority=1, interrupt=True)
            self._enqueue_command(device_id, command)
            self._send_json(
                {
                    "type": "event",
                    "device_id": device_id,
                    "event_type": event_type,
                    "name": name,
                    "morrow_enabled": self._morrow_enabled(),
                    "morrow_skipped": "local_head_touch_expression",
                    "morrow_submitted": False,
                    "queued_commands": [command["cmd_id"]],
                }
            )
            return

        if str(event_type) == "speech_recognition" and not str(details.get("text") or "").strip():
            self._send_json(
                {
                    "type": "event",
                    "device_id": device_id,
                    "event_type": event_type,
                    "name": name,
                    "morrow_enabled": self._morrow_enabled(),
                    "morrow_skipped": "empty_speech_recognition",
                    "morrow_submitted": False,
                    "queued_commands": [],
                }
            )
            self._log_info("语音事件为空，已跳过 Morrow")
            self._log_debug(f"语音事件空结果详情: device={device_id}")
            return

        result = self._send_morrow_event(device_id, str(event_type), details)
        body = {
            "type": "event",
            "device_id": device_id,
            "event_type": event_type,
            "name": name,
            "queued_commands": [],
            **result,
        }
        self._send_json(body)

    def _handle_devices(self):
        now = time.time()
        devices = []
        with self.server.device_lock:
            known_device_ids = list(self.server.device_order)
            last_seen_snapshot = dict(self.server.last_seen)
            last_ack_snapshot = dict(self.server.last_ack)
        for device_id in last_seen_snapshot:
            if device_id not in known_device_ids:
                known_device_ids.append(device_id)
        for device_id in known_device_ids:
            seen = last_seen_snapshot.get(device_id)
            if seen is None:
                continue
            queue = self._queue_for(device_id)
            devices.append(
                {
                    "device_id": device_id,
                    "last_seen_seconds_ago": round(now - seen, 1),
                    "online": now - seen <= DEVICE_ONLINE_TTL_SECONDS,
                    "pending_commands": queue.qsize(),
                    "last_ack": last_ack_snapshot.get(device_id),
                }
            )
        self._send_json(
            {
                "type": "devices",
                "default_device_id": first_connected_device_id(
                    last_seen_snapshot, known_device_ids
                ),
                "online_ttl_seconds": DEVICE_ONLINE_TTL_SECONDS,
                "devices": devices,
                "realtime_devices": getattr(self.server, "realtime_manager", None).devices_snapshot()
                if getattr(self.server, "realtime_manager", None)
                else [],
            }
        )

    def _handle_command(self, query: dict, command_type: str = "", posted: dict | None = None):
        posted = posted or {}
        requested_device_id = first_value(query, "device_id") or posted.get("device_id") or ""
        device_id = self._resolve_command_device_id(requested_device_id)
        command_type = command_type or first_value(query, "type") or posted.get("type") or "speak"
        priority = int(first_value(query, "priority") or posted.get("priority") or 0)
        interrupt = parse_bool(first_value(query, "interrupt") or posted.get("interrupt") or "false")
        ttl_raw = first_value(query, "ttl_seconds") or posted.get("ttl_seconds")
        ttl_seconds = float(ttl_raw) if ttl_raw not in (None, "") else None
        discardable_raw = first_value(query, "discardable")
        discardable = None
        if discardable_raw:
            discardable = parse_bool(discardable_raw)
        elif "discardable" in posted:
            posted_discardable = posted.get("discardable")
            discardable = (
                parse_bool(posted_discardable) if isinstance(posted_discardable, str) else bool(posted_discardable)
            )
        coalesce_key = first_value(query, "coalesce_key") or str(posted.get("coalesce_key") or "")

        if "payload" in posted and isinstance(posted["payload"], (dict, list)):
            payload = posted["payload"]
        else:
            payload = command_payload_from_query(command_type, query)

        if command_type == "action" and isinstance(payload, dict):
            action_name = normalize_expression_name(payload.get("expression") or payload.get("action") or payload.get("face"))
            if action_name in PHYSICAL_ACTIONS:
                command_wire_type = action_name
                payload = {}
            else:
                command_wire_type = "face"
                payload["expression"] = action_name
        elif command_type in ("expression", "action"):
            command_wire_type = "face"
        else:
            command_wire_type = "motion" if command_type == "move" else command_type
        if command_wire_type in ("state", "device_state") and isinstance(payload, dict):
            payload["state"] = normalize_device_state_name(payload.get("state") or payload.get("name") or "waiting")
        elif command_wire_type == "face" and isinstance(payload, dict):
            payload["expression"] = normalize_expression_name(payload.get("expression") or payload.get("face") or "calm")
        elif command_wire_type == "speak" and isinstance(payload, dict):
            payload.setdefault("pause_listener", True)
            payload["expression"] = normalize_expression_name(payload.get("expression") or "calm")
            payload.setdefault("reply_end", True)
        elif command_wire_type == "sequence" and isinstance(payload, list):
            for step in payload:
                if isinstance(step, dict) and step.get("type") == "face":
                    expression = normalize_expression_name(step.get("expression") or step.get("face") or "calm")
                    if expression in PHYSICAL_ACTIONS:
                        step.clear()
                        step["type"] = expression
                    else:
                        step["expression"] = expression
                elif isinstance(step, dict) and step.get("type") == "action":
                    action = normalize_expression_name(step.get("action") or step.get("expression") or "calm")
                    if action in PHYSICAL_ACTIONS:
                        step.clear()
                        step["type"] = action
                    else:
                        step["type"] = "face"
                        step["expression"] = action
                elif isinstance(step, dict) and step.get("type") == "speak":
                    step.setdefault("pause_listener", True)
        normalize_command_speech_payload(command_wire_type, payload)
        if (
            command_wire_type in ("find_owner", "locate_owner")
            and interrupt
            and device_has_pending_dialogue(self.server, device_id)
        ):
            interrupt = False
            self._log_info(
                f"找人命令取消抢占并等待对话结束: device={device_id} type={command_wire_type}"
            )
        if command_wire_type == "stop" and self._morrow_enabled():
            try:
                coordinator = getattr(self.server, "morrow_coordinator", None)
                if coordinator is not None:
                    coordinator.cancel_device(device_id)
            except Exception as exc:
                self._log_error(f"Morrow turn 取消失败: device={device_id} error={exc}")
        command = make_command(
            command_wire_type,
            payload,
            priority=priority,
            interrupt=interrupt,
            ttl_seconds=ttl_seconds,
            discardable=discardable,
            coalesce_key=coalesce_key,
        )
        queued = self._enqueue_command(device_id, command)
        self._send_json({"type": "queued" if queued else "dropped", "device_id": device_id, "command": command})

    def _handle_face_shortcut(self, query: dict, expression: str, action_only: bool = False):
        expression = normalize_expression_name(expression)
        if action_only and expression not in AVAILABLE_ACTIONS:
            self._send_json(
                {
                    "type": "error",
                    "message": f"unknown action: {expression}",
                    "actions": list(AVAILABLE_ACTIONS),
                },
                HTTPStatus.BAD_REQUEST,
            )
            return
        if expression not in AVAILABLE_EXPRESSIONS and expression not in AVAILABLE_ACTIONS:
            self._send_json(
                {
                    "type": "error",
                    "message": f"unknown expression or action: {expression}",
                    "expressions": list(AVAILABLE_EXPRESSIONS),
                    "actions": list(AVAILABLE_ACTIONS),
                },
                HTTPStatus.BAD_REQUEST,
            )
            return

        requested_device_id = first_value(query, "device_id")
        device_id = self._resolve_command_device_id(requested_device_id)
        priority = int(first_value(query, "priority") or 0)
        interrupt = parse_bool(first_value(query, "interrupt") or "false")
        if expression in PHYSICAL_ACTIONS:
            command = make_command(expression, {}, priority=priority, interrupt=interrupt)
        else:
            payload = {"expression": expression}
            command = make_command("face", payload, priority=priority, interrupt=interrupt)
        queued = self._enqueue_command(device_id, command)
        self._send_json(
            {
                "type": "queued" if queued else "dropped",
                "device_id": device_id,
                "expression": expression,
                "kind": "action" if expression in AVAILABLE_ACTIONS else "expression",
                "command": command,
            }
        )

    def _handle_next_command(self, query: dict):
        device_id = self._device_id(query)
        timeout = float(first_value(query, "timeout") or "25")
        timeout = max(0.0, min(timeout, 55.0))
        boot_id = int(first_value(query, "boot_id") or self.headers.get("X-Boot-Id", "0") or "0")
        if boot_id > 0 and not self._observe_device_boot(device_id, boot_id):
            self._mark_device_seen(device_id)
            self._send_json({"type": "noop", "device_id": device_id, "reason": "stale_boot"})
            return
        queue_depth_text = first_value(query, "speech_queue_depth")
        queue_capacity_text = first_value(query, "speech_queue_capacity")
        allow_speak = True
        if queue_depth_text != "" and queue_capacity_text != "":
            try:
                queue_depth = max(0, int(queue_depth_text))
                queue_capacity = max(1, min(64, int(queue_capacity_text)))
                allow_speak = queue_depth < queue_capacity
            except (TypeError, ValueError):
                pass
        if not allow_speak:
            timeout = min(timeout, 1.0)
        def allow_find_owner_now() -> bool:
            return not device_has_pending_dialogue(self.server, device_id)

        if not allow_find_owner_now():
            timeout = min(timeout, 1.0)
        self._mark_device_seen(device_id)
        self._expire_dialog_if_needed(device_id)
        queue = self._queue_for(device_id)
        command_store = getattr(self.server, "command_store", None)
        if command_store is not None:
            leased = command_store.lease_next_command(
                device_id,
                boot_id=boot_id,
                lease_ms=DEFAULT_LEASE_MS,
                allow_speak=allow_speak,
                allow_find_owner=allow_find_owner_now(),
            )
            if leased is not None:
                queue.discard(str(leased.get("cmd_id") or ""))
                self._send_json({"type": "command", "device_id": device_id, "command": leased})
                return
        try:
            command = queue.get(
                timeout=timeout,
                boot_id=boot_id,
                allow_speak=allow_speak,
                allow_find_owner=allow_find_owner_now,
            )
            if command_store is not None:
                leased = command_store.lease_command(
                    str(command.get("cmd_id") or ""),
                    boot_id=boot_id,
                    lease_ms=DEFAULT_LEASE_MS,
                )
                if leased is not None:
                    command = leased
            self._send_json({"type": "command", "device_id": device_id, "command": command})
        except Empty:
            if command_store is not None:
                leased = command_store.lease_next_command(
                    device_id,
                    boot_id=boot_id,
                    lease_ms=DEFAULT_LEASE_MS,
                    allow_speak=allow_speak,
                    allow_find_owner=allow_find_owner_now(),
                )
                if leased is not None:
                    self._send_json({"type": "command", "device_id": device_id, "command": leased})
                    return
            if self._expire_dialog_if_needed(device_id):
                try:
                    command = queue.get_nowait(
                        boot_id=boot_id,
                        allow_speak=allow_speak,
                        allow_find_owner=allow_find_owner_now,
                    )
                    if command_store is not None:
                        leased = command_store.lease_command(
                            str(command.get("cmd_id") or ""),
                            boot_id=boot_id,
                            lease_ms=DEFAULT_LEASE_MS,
                        )
                        if leased is not None:
                            command = leased
                    self._send_json({"type": "command", "device_id": device_id, "command": command})
                    return
                except Empty:
                    pass
            self._send_json({"type": "noop", "device_id": device_id})

    def _device_id(self, query: dict) -> str:
        device_id = first_value(query, "device_id") or self.headers.get("X-Device-Id", "") or "default"
        return safe_device_id(device_id)

    def _resolve_command_device_id(self, requested_device_id: str) -> str:
        device_id = safe_device_id(requested_device_id)
        if is_placeholder_device_id(device_id):
            with self.server.device_lock:
                first_connected = first_connected_device_id(self.server.last_seen, self.server.device_order)
            if first_connected:
                return first_connected
            manager = getattr(self.server, "realtime_manager", None)
            if manager:
                realtime_device = manager.first_device_id()
                if not is_placeholder_device_id(realtime_device):
                    return realtime_device
        return device_id

    def _queue_for(self, device_id: str) -> DeviceCommandQueue:
        with self.server.device_lock:
            queue = self.server.device_queues.get(device_id)
            if queue is None:
                queue = DeviceCommandQueue(self.server.command_queue_max_size)
                self.server.device_queues[device_id] = queue
            return queue

    def _enqueue_command(self, device_id: str, command: dict) -> bool:
        device_id = safe_device_id(device_id)
        return self._enqueue_http_command(device_id, command)

    def _enqueue_http_command(self, device_id: str, command: dict) -> bool:
        device_id = safe_device_id(device_id)
        command_type = str(command.get("type") or "")
        payload = command.get("payload")
        apply_current_speech_generation(self.server, device_id, command_type, payload)
        prepare_server_command_audio(self.server, command_type, payload)
        command_store = getattr(self.server, "command_store", None)
        if command_store is not None:
            try:
                command["boot_id"] = command_store.current_boot_id(device_id)
                envelope = CommandEnvelope.from_legacy(device_id, command)
                command_store.create_command(envelope)
            except Exception as exc:
                self._log_error(f"命令持久化失败: device={device_id} cmd_id={command.get('cmd_id', '')} error={exc}")
                return False
        queue = self._queue_for(device_id)
        stats = queue.put(command)
        detail = ""
        if command.get("type") == "face" and isinstance(command.get("payload"), dict):
            detail = f" expression={command['payload'].get('expression', '')}"
        if stats.get("queued"):
            self._log_info(f"命令已入队: type={command['type']}{detail} priority={command.get('priority')}")
        else:
            self._log_info(f"命令已丢弃: type={command['type']}{detail} priority={command.get('priority')}")
        self._log_debug(
            f"命令队列详情: device={device_id} cmd_id={command['cmd_id']} "
            f"type={command['type']}{detail} stats={stats}"
        )
        return bool(stats.get("queued"))

    def _mark_device_seen(self, device_id: str) -> None:
        device_id = safe_device_id(device_id)
        now = time.time()
        first_seen = False
        with self.server.device_lock:
            if device_id not in self.server.device_order:
                self.server.device_order.append(device_id)
                first_seen = True
            self.server.last_seen[device_id] = now
        if first_seen:
            event = {
                "type": "connected",
                "source": "server",
                "device_id": device_id,
                "server_ts": now,
                "message": "服务端首次看到设备",
            }
            try:
                reset_device_log_file(self.server, device_id, event)
            except OSError as exc:
                self._log_error(f"设备日志文件重置失败: device={device_id} error={exc}")

    def _dialog_awake(self, device_id: str) -> bool:
        device_id = safe_device_id(device_id)
        if self._expire_dialog_if_needed(device_id):
            return False
        with self.server.device_lock:
            return time.time() < self.server.dialog_awake_until.get(device_id, 0)

    def _expire_dialog_if_needed(self, device_id: str) -> bool:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            awake_until = self.server.dialog_awake_until.get(device_id, 0)
        if awake_until > 0 and time.time() >= awake_until:
            self._sleep_dialog(device_id, reason="timeout")
            return True
        return False

    def _wake_dialog(self, device_id: str, reason: str = "") -> None:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            self.server.dialog_awake_until[device_id] = time.time() + DIALOG_AWAKE_SECONDS
        self._log_info("对话已唤醒")
        self._log_debug(f"对话唤醒详情: device={device_id} ttl={DIALOG_AWAKE_SECONDS}s reason={reason!r}")

    def _sleep_dialog(self, device_id: str, reason: str = "") -> None:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            self.server.dialog_awake_until[device_id] = 0
        self._send_device_state_command(device_id, "sleep", reason=reason)
        self._log_info("对话已休眠")
        self._log_debug(f"对话休眠详情: device={device_id} reason={reason!r}")

    def _morrow_enabled(self) -> bool:
        return getattr(self.server, "morrow_coordinator", None) is not None

    def _morrow_health(self) -> dict:
        client = getattr(self.server, "morrow_client", None)
        coordinator = getattr(self.server, "morrow_coordinator", None)
        return {
            "base_url": getattr(client, "base_url", ""),
            "session": getattr(client, "session", "default"),
            "connected": bool(client and client.connected),
            "snapshot_received": bool(client and client.ready),
            "active_request_id": coordinator.active_request_id if coordinator else "",
            "queued_turns": coordinator.queued_turns if coordinator else 0,
            "last_message_at": getattr(client, "last_message_at", 0),
            "last_notice_at": getattr(client, "last_notice_at", 0),
            "last_error": getattr(client, "last_error", ""),
            "metrics": {**getattr(client, "metrics", {}), **getattr(coordinator, "metrics", {})},
        }

    def _command_health(self) -> dict:
        with self.server.v3_database.connect() as conn:
            return {
                state: conn.execute("SELECT COUNT(*) FROM commands WHERE state=?", (state,)).fetchone()[0]
                for state in ("queued", "leased", "running", "expired")
            }

    def _device_health(self) -> dict:
        with self.server.v3_database.connect() as conn:
            return {"online": conn.execute("SELECT COUNT(*) FROM devices WHERE online=1").fetchone()[0]}

    def _handle_metrics(self) -> None:
        client = getattr(self.server, "morrow_client", None)
        coordinator = getattr(self.server, "morrow_coordinator", None)
        metrics = {**getattr(client, "metrics", {}), **getattr(coordinator, "metrics", {})}
        for name in (
            "morrow_notice_duplicate_total",
            "morrow_notice_rendered_total",
            "morrow_notice_expired_total",
            "device_command_retry_total",
            "device_ack_replay_total",
            "speech_queue_full_total",
        ):
            metrics.setdefault(name, 0)
        with self.server.v3_database.connect() as conn:
            for state in ("queued", "leased", "running", "expired"):
                metrics[f"device_commands_{state}"] = conn.execute(
                    "SELECT COUNT(*) FROM commands WHERE state=?", (state,)
                ).fetchone()[0]
            metrics["devices_online"] = conn.execute("SELECT COUNT(*) FROM devices WHERE online=1").fetchone()[0]
        data = ("\n".join(f"{name} {value}" for name, value in sorted(metrics.items())) + "\n").encode()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_device_state_command(self, device_id: str, state: str, reason: str = "") -> list[str]:
        device_id = safe_device_id(device_id)
        state = str(state or "").strip()
        if not state:
            return []
        command = make_command(
            "state",
            {"state": state, "reason": reason},
            priority=88,
            interrupt=True,
            ttl_seconds=8,
            discardable=True,
            coalesce_key="device_state",
        )
        if self._enqueue_command(device_id, command):
            return [command["cmd_id"]]
        return []

    def _enter_morrow_waiting(self, device_id: str, event_type: str) -> list[str]:
        # Waiting for Morrow/TTS keeps the normal calm + blink face.  The
        # reply expression starts only when the first PCM block is queued.
        return self._send_device_state_command(device_id, "waiting", reason=f"morrow:{event_type}")

    def _send_morrow_event(self, device_id: str, event_type: str, details: dict) -> dict:
        if not self._morrow_enabled():
            return {"morrow_enabled": False, "morrow_submitted": False, "queued_commands": []}

        queued_commands = self._enter_morrow_waiting(device_id, event_type)

        coordinator = getattr(self.server, "morrow_coordinator", None)
        if coordinator is not None:
            content = build_morrow_event_content(device_id, event_type, details)
            source = "voice" if event_type == "speech_recognition" else "system"
            try:
                outcome = coordinator.submit(content, device_id, source=source)
            except Exception as exc:
                self._log_error(f"Morrow 请求入队失败: device={device_id} event={event_type} error={exc}")
                return {
                    "morrow_enabled": True,
                    "morrow_submitted": False,
                    "queued_commands": queued_commands,
                    "error": str(exc),
                }
            self._log_info(f"Morrow 请求已进入全局队列: request_id={outcome.request_id} event={event_type}")
            return {
                "morrow_enabled": True,
                "morrow_submitted": True,
                "morrow_request_id": outcome.request_id,
                "queued_commands": queued_commands,
            }

        return {
            "morrow_enabled": False,
            "morrow_submitted": False,
            "queued_commands": queued_commands,
        }

    def _handle_tts_voices(self) -> None:
        self._send_json(
            {
                "type": "tts_voices",
                "current_voice": self.server.voice,
                "default_sample_rate": self.server.sample_rate,
                "voices": list(ALIYUN_TTS_DEBUG_VOICES),
                "note": "This is a curated debug list. The debug synthesis endpoint accepts any voice supported by Aliyun NLS.",
                "source": ALIYUN_TTS_VOICE_DOC_URL,
                "debug_endpoint": f"/tts/debug?text=你好&voice={DEFAULT_TTS_VOICE}&format=wav",
            }
        )

    def _tts_request_params(self, query: dict, body: bytes | None = None) -> dict:
        params: dict = {}
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if body:
            if content_type == "application/json":
                payload = json.loads(body.decode("utf-8"))
                if isinstance(payload, dict):
                    params.update(payload)
            else:
                params["text"] = body.decode("utf-8")
        for key in ("text", "input", "voice", "sample_rate", "volume", "speech_rate", "pitch_rate", "format", "audio_format"):
            value = first_value(query, key)
            if value != "":
                params[key] = value
        if "text" not in params and "input" in params:
            params["text"] = params["input"]
        return params

    def _handle_tts_debug(self, query: dict, body: bytes | None = None) -> None:
        try:
            params = self._tts_request_params(query, body)
            options = tts_request_options_from_params(self.server, params)
        except (json.JSONDecodeError, ValueError) as exc:
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_REQUEST)
            return

        text = normalize_speech_text_for_voice(str(params.get("text") or ""))
        if not text:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing text")
            return
        parts = list(split_sentences(text, self.server.max_sentence_chars))
        if not parts:
            self.send_error(HTTPStatus.BAD_REQUEST, "empty text")
            return

        started = time.perf_counter()
        try:
            audio_parts = []
            for part in parts:
                self._log_info(f"TTS 调试合成: voice={options.voice} format={options.audio_format} text={part!r}")
                audio_parts.append(self._aliyun_tts_pcm_with_retries(part, options))
            pcm = b"".join(audio_parts) + self._tts_tail_silence(options.sample_rate)
        except Exception as exc:
            self._log_error(f"TTS 调试失败: {exc}")
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        if options.audio_format == "wav":
            audio = pcm_to_wav(pcm, options.sample_rate)
            content_type = "audio/wav"
            audio_header = "wav"
        else:
            audio = pcm
            content_type = "application/octet-stream"
            audio_header = "pcm_s16le"

        elapsed_ms = (time.perf_counter() - started) * 1000
        self._log_info(
            f"TTS 调试成功: voice={options.voice} format={options.audio_format} "
            f"sentences={len(parts)} bytes={len(audio)} elapsed_ms={elapsed_ms:.0f}"
        )
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(audio)))
        self.send_header("X-Audio-Format", audio_header)
        self.send_header("X-Sample-Rate", str(options.sample_rate))
        self.send_header("X-Channels", "1")
        self.send_header("X-TTS-Voice", options.voice)
        self.send_header("X-TTS-Volume", str(options.volume))
        self.send_header("X-TTS-Speech-Rate", str(options.speech_rate))
        self.send_header("X-TTS-Pitch-Rate", str(options.pitch_rate))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(audio)

    def _handle_event_audio(self, filename: str):
        audio_ext = "wav" if filename.endswith(".wav") else "pcm"
        name = filename.rsplit(".", 1)[0] if "." in filename else filename
        if name not in EVENT_AUDIO_TEXT:
            self._send_json(
                {
                    "type": "error",
                    "message": f"unknown event audio: {name}",
                    "events": list(EVENT_AUDIO_TEXT),
                },
                HTTPStatus.NOT_FOUND,
            )
            return

        try:
            pcm_path, wav_path = ensure_event_audio_cache(self.server, name, logger=self._log_info)
        except Exception as exc:
            self._log_error(f"事件音频 TTS 失败: {exc}")
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        path = wav_path if audio_ext == "wav" else pcm_path

        try:
            stat = os.stat(path)
            self._log_info(f"事件音频已发送: name={name} format={audio_ext} bytes={stat.st_size}")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "audio/wav" if audio_ext == "wav" else "application/octet-stream")
            self.send_header("Content-Length", str(stat.st_size))
            self.send_header("X-Audio-Format", "wav" if audio_ext == "wav" else "pcm_s16le")
            self.send_header("X-Event-Audio-Name", name)
            self.send_header("X-Event-Audio-Cache", "hit")
            self.send_header("X-Sample-Rate", str(self.server.sample_rate))
            self.send_header("X-Channels", "1")
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
            self.end_headers()
            with open(path, "rb") as fp:
                while True:
                    chunk = fp.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            self._log_info(f"事件音频客户端已断开: name={name}")

    def _handle_upload_image(self, body: bytes):
        if not body:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing image body")
            return

        content_type = self.headers.get("Content-Type", "application/octet-stream")
        width = int(self.headers.get("X-Image-Width", "0") or "0")
        height = int(self.headers.get("X-Image-Height", "0") or "0")
        image_format = self.headers.get("X-Image-Format", "").strip().lower()
        device_id = self.headers.get("X-Device-Id", "unknown")
        device_id = safe_device_id(device_id)
        visual_tracking_requested = parse_bool(self.headers.get("X-Visual-Tracking", "true"))
        self._mark_device_seen(device_id)
        safe_device = re.sub(r"[^A-Za-z0-9_.-]+", "_", device_id)[:40] or "unknown"

        os.makedirs(self.server.capture_dir, exist_ok=True)
        stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        base = os.path.join(self.server.capture_dir, f"xiaopai-{safe_device}-{stamp}")
        save_mode = getattr(self.server, "capture_save_mode", "raw")
        save_raw = save_mode in ("raw", "debug") or self.server.face_detector_backend == "legacy"
        save_debug = save_mode == "debug" or self.server.face_detector_backend == "legacy"
        save_visual = save_mode == "debug"

        raw_ext = "jpg" if content_type.startswith("image/jpeg") else (image_format or "bin")
        raw_path = f"{base}.{raw_ext}" if save_raw else ""
        if raw_path:
            with open(raw_path, "wb") as fp:
                fp.write(body)

        bmp_path = ""
        png_path = ""
        face_visual_path = ""
        face_result = {"available": False, "faces": []}
        if image_format == "rgb565" and width > 0 and height > 0:
            expected = width * height * 2
            if len(body) != expected:
                self._send_json(
                    {
                        "type": "error",
                        "message": f"rgb565 size mismatch: got {len(body)}, expected {expected}",
                        "raw_path": raw_path,
                    },
                    HTTPStatus.BAD_REQUEST,
                )
                return
            if save_debug:
                bmp_path = f"{base}.bmp"
                with open(bmp_path, "wb") as fp:
                    fp.write(rgb565_to_bmp(body, width, height))
                png_path = f"{base}.png"
                with open(png_path, "wb") as fp:
                    fp.write(rgb565_to_png(body, width, height))
            if self.server.face_detector is not None:
                face_visual_path, face_result = self.server.face_detector.detect_rgb565(
                    body,
                    width,
                    height,
                    f"{base}.faces.jpg" if save_visual else "",
                )
            elif self.server.face_detector_backend == "legacy":
                if not png_path:
                    png_path = f"{base}.png"
                    with open(png_path, "wb") as fp:
                        fp.write(rgb565_to_png(body, width, height))
                face_visual_path, face_result = detect_and_visualize_faces(png_path, f"{base}.faces.png")
        elif image_format == "yuv422" and width > 0 and height > 0:
            expected = width * height * 2
            if len(body) != expected or width % 2 != 0:
                self._send_json(
                    {
                        "type": "error",
                        "message": f"yuv422 size mismatch: got {len(body)}, expected {expected}, width={width}",
                        "raw_path": raw_path,
                    },
                    HTTPStatus.BAD_REQUEST,
                )
                return
            if save_debug:
                png_path = f"{base}.png"
                with open(png_path, "wb") as fp:
                    fp.write(yuv422_to_png(body, width, height))
            if self.server.face_detector is not None:
                face_visual_path, face_result = self.server.face_detector.detect_yuv422(
                    body,
                    width,
                    height,
                    f"{base}.faces.jpg" if save_visual else "",
                )
            elif self.server.face_detector_backend == "legacy":
                if not png_path:
                    png_path = f"{base}.png"
                    with open(png_path, "wb") as fp:
                        fp.write(yuv422_to_png(body, width, height))
                face_visual_path, face_result = detect_and_visualize_faces(png_path, f"{base}.faces.png")
        elif content_type.startswith("image/jpeg"):
            if self.server.face_detector is not None:
                face_visual_path, face_result = self.server.face_detector.detect_jpeg(
                    body,
                    f"{base}.faces.jpg" if save_visual else "",
                )
            elif self.server.face_detector_backend == "legacy":
                face_visual_path, face_result = detect_and_visualize_faces(raw_path, f"{base}.faces.png")

        if visual_tracking_requested:
            tracking_command = self._maybe_enqueue_visual_tracking(device_id, width, height, face_result)
        else:
            tracking_command = {"status": "suppressed"}
        self._log_info(
            "图片上传已处理: "
            f"{width}x{height}, faces={len(face_result.get('faces', []))}, "
            f"tracking={tracking_command.get('status', 'none')}"
        )
        self._log_debug(
            f"图片上传详情: bytes={len(body)} type={content_type} format={image_format} "
            f"size={width}x{height} raw={raw_path} bmp={bmp_path} png={png_path} "
            f"face_visual={face_visual_path} tracking={compact_log_json(tracking_command)}"
        )
        self._send_json(
            {
                "type": "image",
                "bytes": len(body),
                "format": image_format or content_type,
                "width": width,
                "height": height,
                "raw_path": raw_path,
                "bmp_path": bmp_path,
                "png_path": png_path,
                "face_visual_path": face_visual_path,
                "face_detection": face_result,
                "visual_tracking": tracking_command,
            }
        )

    def _maybe_enqueue_visual_tracking(self, device_id: str, width: int, height: int, face_result: dict) -> dict:
        if not self.server.visual_tracking_enabled:
            return {"status": "disabled"}
        if not face_result.get("available"):
            return {"status": "no_detector", "message": face_result.get("error", "")}
        best_face = face_result.get("best_face")
        if not isinstance(best_face, dict):
            return {"status": "no_face"}

        detect_width = float(face_result.get("width") or width or 0)
        detect_height = float(face_result.get("height") or height or 0)
        if detect_width <= 0 or detect_height <= 0:
            return {"status": "bad_frame_size"}
        center = best_face.get("center") if isinstance(best_face.get("center"), dict) else {}
        face_x = float(center.get("x", detect_width / 2.0))
        face_y = float(center.get("y", detect_height / 2.0))
        error_x = face_x - detect_width / 2.0
        error_y = face_y - detect_height / 2.0
        if self.server.visual_tracking_invert_x:
            error_x = -error_x
        if self.server.visual_tracking_invert_y:
            error_y = -error_y
        deadzone = float(self.server.visual_tracking_deadzone_px)

        steps = []
        if abs(error_x) > deadzone:
            steps.append(
                {
                    "type": "motion",
                    "direction": "right" if error_x > 0 else "left",
                    "degree": self._visual_tracking_degree(
                        abs(error_x), detect_width / 2.0, self.server.visual_tracking_gain_x
                    ),
                    "duration_ms": self.server.visual_tracking_duration_ms,
                }
            )
        if abs(error_y) > deadzone:
            steps.append(
                {
                    "type": "motion",
                    "direction": "down" if error_y > 0 else "up",
                    "degree": self._visual_tracking_degree(
                        abs(error_y), detect_height / 2.0, self.server.visual_tracking_gain_y
                    ),
                    "duration_ms": self.server.visual_tracking_duration_ms,
                }
            )
        if not steps:
            return {
                "status": "centered",
                "target": {"x": face_x, "y": face_y},
                "error": {"x": error_x, "y": error_y},
            }

        queue = self._queue_for(device_id)
        if queue.qsize() >= self.server.visual_tracking_max_pending:
            return {
                "status": "skipped_queue_full",
                "pending_commands": queue.qsize(),
                "max_pending": self.server.visual_tracking_max_pending,
            }

        now = time.time()
        last_command_at = self.server.visual_tracking_last_command_at.get(device_id, 0.0)
        min_interval = self.server.visual_tracking_min_interval_ms / 1000.0
        if now - last_command_at < min_interval:
            return {
                "status": "skipped_rate_limited",
                "elapsed_ms": round((now - last_command_at) * 1000.0, 1),
                "min_interval_ms": self.server.visual_tracking_min_interval_ms,
            }

        if len(steps) == 1:
            payload = steps[0]
            payload = {key: value for key, value in payload.items() if key != "type"}
            command = make_command("motion", payload, priority=0, interrupt=False)
        else:
            command = make_command("sequence", steps, priority=0, interrupt=False)
        self._enqueue_command(device_id, command)
        self.server.visual_tracking_last_command_at[device_id] = now
        return {
            "status": "queued",
            "device_id": device_id,
            "cmd_id": command["cmd_id"],
            "command": command,
            "target": {"x": face_x, "y": face_y},
            "error": {"x": error_x, "y": error_y},
        }

    def _visual_tracking_degree(self, abs_error_px: float, half_dimension_px: float, gain: float) -> float:
        if half_dimension_px <= 0:
            return self.server.visual_tracking_min_degree
        ratio = min(1.0, abs_error_px / half_dimension_px)
        degree = ratio * self.server.visual_tracking_max_degree * gain
        degree = max(self.server.visual_tracking_min_degree, min(self.server.visual_tracking_max_degree, degree))
        return round(degree, 1)

    def _aliyun_asr(self, audio: bytes, audio_format: str, sample_rate: int):
        params = {
            "appkey": self.server.appkey,
            "format": audio_format,
            "sample_rate": str(sample_rate),
            "enable_punctuation_prediction": "true",
            "enable_inverse_text_normalization": "true",
            "enable_voice_detection": "true",
        }
        url = self.server.asr_url + "?" + urllib.parse.urlencode(params)
        req = urllib.request.Request(
            url,
            data=audio,
            method="POST",
            headers={
                "X-NLS-Token": self.server.get_token(),
                "Content-Type": "application/octet-stream",
            },
        )
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def _handle_stream_speak(self, query: dict, body: bytes | None = None):
        try:
            params = self._tts_request_params(query, body)
            params["format"] = "pcm"
            options = tts_request_options_from_params(self.server, params)
            if options.sample_rate != 24000:
                raise ValueError("device TTS only supports 24000 Hz PCM")
        except (json.JSONDecodeError, ValueError) as exc:
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_REQUEST)
            return

        text = normalize_speech_text_for_voice(str(params.get("text") or ""))
        if not text:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing text")
            return
        if speech_text_is_temporarily_suppressed(text):
            self._log_info("TTS 已被临时静默保护抑制")
            self.send_response(HTTPStatus.NO_CONTENT)
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        parts = list(split_sentences(text, self.server.max_sentence_chars))
        if not parts:
            self.send_error(HTTPStatus.BAD_REQUEST, "empty text")
            return

        stream_started = time.perf_counter()
        prefetch_pool: ThreadPoolExecutor | None = None
        prefetch_futures = []
        try:
            self._log_info(
                f"TTS 准备实时流: voice={options.voice} sentences={len(parts)} text={text!r}"
            )
            first_response = self._open_aliyun_tts_stream_with_retries(parts[0], options)
        except Exception as exc:
            self._log_error(f"TTS 在响应开始前失败: {exc}")
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        ready_ms = (time.perf_counter() - stream_started) * 1000
        self._log_info(
            f"TTS 实时流已就绪: voice={options.voice} sentences={len(parts)} "
            f"ready_ms={ready_ms:.0f}, text={text!r}"
        )
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("X-Audio-Format", "pcm_s16le")
        self.send_header("X-Sample-Rate", str(options.sample_rate))
        self.send_header("X-Channels", "1")
        self.send_header("X-TTS-Voice", options.voice)
        self.send_header("X-TTS-Volume", str(options.volume))
        self.send_header("X-TTS-Speech-Rate", str(options.speech_rate))
        self.send_header("X-TTS-Pitch-Rate", str(options.pitch_rate))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        total_bytes = 0
        first_bytes = 0
        try:
            if len(parts) > 1:
                workers = max(1, min(self.server.tts_prefetch_workers, len(parts) - 1))
                prefetch_pool = ThreadPoolExecutor(max_workers=workers)
                prefetch_futures = [
                    (part, prefetch_pool.submit(self._aliyun_tts_pcm_with_retries, part, options))
                    for part in parts[1:]
                ]

            chunk_size = max(512, int(getattr(self.server, "chunk_size", 4096) or 4096))
            first_bytes = self._copy_pcm_stream_to_client(first_response, chunk_size)
            total_bytes += first_bytes
            self._log_info(f"TTS 已发送首句: bytes={first_bytes} text={parts[0]!r}")

            future_timeout = self.server.tts_request_timeout * (self.server.tts_retries + 1) + 5
            for part, future in prefetch_futures:
                wait_started = time.perf_counter()
                audio = future.result(timeout=future_timeout)
                wait_ms = (time.perf_counter() - wait_started) * 1000
                if audio:
                    self.wfile.write(audio)
                    self.wfile.flush()
                    total_bytes += len(audio)
                self._log_info(f"TTS 已发送预取句子: bytes={len(audio)} wait_ms={wait_ms:.0f} text={part!r}")

            tail_silence = self._tts_tail_silence(options.sample_rate)
            if tail_silence:
                self.wfile.write(tail_silence)
                self.wfile.flush()
                total_bytes += len(tail_silence)
            total_ms = (time.perf_counter() - stream_started) * 1000
            self._log_info(f"TTS 实时流完成: bytes={total_bytes} first_bytes={first_bytes} total_ms={total_ms:.0f}")
        except (BrokenPipeError, ConnectionResetError):
            self._log_info("TTS 客户端已断开")
        except Exception as exc:
            self._log_error(f"TTS 在流开始后失败: {exc}")
        finally:
            try:
                first_response.close()
            except Exception:
                pass
            if prefetch_pool is not None:
                prefetch_pool.shutdown(wait=False, cancel_futures=True)

    def _copy_pcm_stream_to_client(self, response, chunk_size: int) -> int:
        total_bytes = 0
        while True:
            chunk = response.read(chunk_size)
            if not chunk:
                break
            self.wfile.write(chunk)
            self.wfile.flush()
            total_bytes += len(chunk)
        return total_bytes

    def _aliyun_tts_pcm_with_retries(self, text: str, options: TtsRequestOptions | None = None) -> bytes:
        last_error: Exception | None = None
        for attempt in range(1, self.server.tts_retries + 2):
            try:
                return self._aliyun_tts_pcm(text, options)
            except Exception as exc:
                last_error = exc
                self._log_error(f"TTS 第 {attempt} 次尝试失败: text={text!r} error={exc}")
        raise RuntimeError(f"Aliyun TTS failed after {self.server.tts_retries + 1} attempt(s): {last_error}")

    def _tts_tail_silence(self, sample_rate: int | None = None) -> bytes:
        ms = max(0, int(self.server.tts_tail_silence_ms))
        samples = int(sample_rate or self.server.sample_rate) * ms // 1000
        return b"\x00\x00" * samples

    def _aliyun_tts_request(self, text: str, options: TtsRequestOptions | None = None):
        voice = options.voice if options is not None else self.server.voice
        sample_rate = options.sample_rate if options is not None else self.server.sample_rate
        volume = options.volume if options is not None else self.server.volume
        speech_rate = options.speech_rate if options is not None else self.server.speech_rate
        pitch_rate = options.pitch_rate if options is not None else self.server.pitch_rate
        params = {
            "appkey": self.server.appkey,
            "token": self.server.get_token(),
            "text": text,
            "format": "pcm",
            "sample_rate": sample_rate,
            "voice": voice,
            "volume": volume,
            "speech_rate": speech_rate,
            "pitch_rate": pitch_rate,
        }
        url = self.server.tts_url + "?" + urllib.parse.urlencode(params)
        return urllib.request.Request(url, method="GET")

    def _open_aliyun_tts_stream_with_retries(self, text: str, options: TtsRequestOptions | None = None):
        last_error: Exception | None = None
        for attempt in range(1, self.server.tts_retries + 2):
            try:
                return self._open_aliyun_tts_stream(text, options)
            except Exception as exc:
                last_error = exc
                self._log_error(f"TTS 打开流第 {attempt} 次尝试失败: text={text!r} error={exc}")
        raise RuntimeError(f"Aliyun TTS stream failed after {self.server.tts_retries + 1} attempt(s): {last_error}")

    def _open_aliyun_tts_stream(self, text: str, options: TtsRequestOptions | None = None):
        started = time.perf_counter()
        req = self._aliyun_tts_request(text, options)
        try:
            resp = urllib.request.urlopen(req, timeout=self.server.tts_request_timeout)
            content_type = resp.headers.get("Content-Type", "")
            if "json" in content_type:
                detail = resp.read().decode("utf-8", errors="replace")
                resp.close()
                raise RuntimeError(detail)
            elapsed_ms = (time.perf_counter() - started) * 1000
            self._log_info(f"阿里云 TTS 流已打开: chars={len(text)} elapsed_ms={elapsed_ms:.0f}")
            return resp
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Aliyun TTS HTTP {exc.code}: {detail}") from exc

    def _aliyun_tts_pcm(self, text: str, options: TtsRequestOptions | None = None) -> bytes:
        started = time.perf_counter()
        req = self._aliyun_tts_request(text, options)
        try:
            with urllib.request.urlopen(req, timeout=self.server.tts_request_timeout) as resp:
                content_type = resp.headers.get("Content-Type", "")
                if "json" in content_type:
                    raise RuntimeError(resp.read().decode("utf-8", errors="replace"))
                audio = resp.read()
                elapsed_ms = (time.perf_counter() - started) * 1000
                self._log_info(f"阿里云 TTS 成功: chars={len(text)} bytes={len(audio)} elapsed_ms={elapsed_ms:.0f}")
                return audio
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Aliyun TTS HTTP {exc.code}: {detail}") from exc

    def _send_json(self, body: dict, status: HTTPStatus = HTTPStatus.OK):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self._log_api_result(body, status)

    def _log_api_result(self, body: dict, status: HTTPStatus) -> None:
        path, _query = self._path_query()
        if not (
            path == "/command"
            or path.startswith("/command/")
            or path.startswith("/action/")
            or path.startswith("/expression/")
            or path in ("/device/event", "/event")
        ):
            return
        if self._debug_enabled():
            self._log_debug(
                "API 结果详情: "
                f"method={self.command} path={self.path!r} status={int(status)} "
                f"body={compact_log_json(body)}"
            )
            return
        if path == "/command" or path.startswith("/command/"):
            command = body.get("command") if isinstance(body.get("command"), dict) else {}
            self._log_info(f"API 命令响应: {body.get('type', 'response')} {command.get('type', '')}".rstrip())
            return
        self._log_info(f"API 响应: {path} -> {int(status)}")

    def log_message(self, fmt, *args):
        if self._debug_enabled():
            self._log_debug(f"HTTP 请求: client={self.client_address[0]} raw={fmt % args}")
            return
        parsed = urllib.parse.urlparse(getattr(self, "path", ""))
        code = args[1] if len(args) > 1 else ""
        suffix = f" -> {code}" if code else ""
        self._log_info(f"HTTP 请求: method={getattr(self, 'command', '')} path={parsed.path}{suffix}")


def required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Missing {name}. Export it before starting this service.")
    return value


def truncate_log_text(value: str, limit: int = LOG_TEXT_MAX_CHARS) -> str:
    text = str(value or "")
    if len(text) <= limit:
        return text
    return text[:limit] + f"...<truncated {len(text) - limit} chars>"


def compact_log_json(value) -> str:
    try:
        text = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    except (TypeError, ValueError):
        text = repr(value)
    return truncate_log_text(text)


def device_log_file_path(device_log_dir: str, device_id: str) -> str:
    safe_device = re.sub(r"[^A-Za-z0-9_.-]+", "_", safe_device_id(device_id))[:64] or "default"
    return os.path.join(str(device_log_dir), f"{safe_device}.log")


def _device_log_timestamp(value) -> str:
    try:
        ts = float(value)
    except (TypeError, ValueError):
        ts = time.time()
    return _dt.datetime.fromtimestamp(ts).isoformat(timespec="milliseconds")


DEVICE_LOG_TOKEN_LABELS = {
    "log": "日志",
    "connected": "连接",
    "state": "状态",
    "state_change": "状态变化",
    "server": "服务端",
    "esp-log": "设备日志",
    "xiaopai-state": "小派状态",
    "expression-state": "表情状态",
    "realtime-listen": "实时收音",
    "http-upload": "HTTP 上传",
    "idle": "空闲",
    "listening": "监听中",
    "waiting": "等待中",
    "speaking": "播放中",
    "sleep": "休眠",
    "sleeping": "休眠中",
    "calm": "平静",
    "calm_blink": "平静眨眼",
    "sleep_dark": "休眠黑屏",
    "shy": "害羞",
    "thinking": "思考中",
    "relaxed": "放松",
}


def device_log_token_label(value) -> str:
    text = str(value or "").strip()
    return DEVICE_LOG_TOKEN_LABELS.get(text, text)


def _device_log_value(value) -> str:
    return compact_log_json(str(value))


def format_device_log_line(event: dict) -> str:
    item = dict(event or {})
    event_type = str(item.get("type") or item.get("event_type") or "log")
    parts = [
        f"[{_device_log_timestamp(item.get('server_ts'))}]",
        f"类型={device_log_token_label(event_type)}",
    ]
    device_id = str(item.get("device_id") or "").strip()
    if device_id:
        parts.append(f"设备={safe_device_id(device_id)}")
    source = str(item.get("source") or item.get("state_machine") or "").strip()
    if source:
        parts.append(f"来源={device_log_token_label(source)}")
    if item.get("device_ms") not in (None, ""):
        parts.append(f"设备毫秒={item.get('device_ms')}")

    if event_type in ("state", "state_change"):
        old = item.get("from") or item.get("old") or ""
        new = item.get("to") or item.get("new") or item.get("state") or ""
        if old or new:
            parts.append(f"状态={device_log_token_label(old)}->{device_log_token_label(new)}")
        if item.get("reason"):
            parts.append(f"原因={_device_log_value(item.get('reason'))}")

    message = item.get("line")
    if message in (None, ""):
        message = item.get("message")
    if message in (None, ""):
        message = item.get("text")
    if message not in (None, ""):
        parts.append(f"消息={_device_log_value(message)}")

    known = {
        "type",
        "event_type",
        "device_id",
        "server_ts",
        "source",
        "state_machine",
        "device_ms",
        "line",
        "message",
        "text",
        "from",
        "old",
        "to",
        "new",
        "state",
        "reason",
    }
    extra = {key: item[key] for key in sorted(item) if key not in known and item[key] not in (None, "")}
    if extra:
        parts.append(f"额外={compact_log_json(extra)}")
    return " ".join(parts)


def append_device_log_file(server, device_id: str, event: dict) -> None:
    device_log_dir = str(getattr(server, "device_log_dir", "") or "")
    if not device_log_dir:
        return
    os.makedirs(device_log_dir, exist_ok=True)
    path = device_log_file_path(device_log_dir, device_id)
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(format_device_log_line(event).rstrip("\n") + "\n")


def reset_device_log_file(server, device_id: str, event: dict) -> None:
    device_log_dir = str(getattr(server, "device_log_dir", "") or "")
    if not device_log_dir:
        return
    os.makedirs(device_log_dir, exist_ok=True)
    path = device_log_file_path(device_log_dir, device_id)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(format_device_log_line(event).rstrip("\n") + "\n")


def reset_device_logs_for_reconnect(server, device_id: str, *, reason: str = "实时 WebSocket 已连接") -> None:
    device_id = safe_device_id(device_id)
    if is_placeholder_device_id(device_id):
        return
    now = time.time()
    event = {
        "type": "connected",
        "source": "server",
        "device_id": device_id,
        "server_ts": now,
        "message": reason,
    }
    with server.device_lock:
        if device_id not in server.device_order:
            server.device_order.append(device_id)
        server.last_seen[device_id] = now
        server.device_logs[device_id] = [event]
    try:
        reset_device_log_file(server, device_id, event)
    except OSError as exc:
        log_print(f"设备日志重置失败: device={device_id} error={exc}")


def build_morrow_event_text(device_id: str, event_type: str, details: dict) -> str:
    device_id = safe_device_id(device_id)
    source_event_type = str(event_type or "event").strip() or "event"
    compact_details = {str(key): value for key, value in (details or {}).items() if value not in (None, "")}
    if source_event_type == "speech_recognition":
        return str(compact_details.get("text") or "").strip()

    text = str(compact_details.get("text") or "").strip()
    if text:
        return text

    name = str(compact_details.get("name") or compact_details.get("event") or "").strip()
    parts = [f"设备 {device_id}", f"事件类型 {source_event_type}"]
    if name:
        parts.append(f"事件名称 {name}")

    extra_parts = []
    for key in sorted(compact_details):
        if key in ("context", "device_id", "event", "event_id", "id", "name", "text", "timestamp", "ts", "user_id"):
            continue
        value = compact_details[key]
        if isinstance(value, (str, int, float, bool)):
            extra_parts.append(f"{key}={value}")

    message = "小派设备事件：" + "，".join(parts)
    if extra_parts:
        message += "；详情：" + "，".join(extra_parts)
    return message + "。"


def build_morrow_event_content(device_id: str, event_type: str, details: dict) -> str:
    return build_morrow_event_text(device_id, event_type, details)


def optional_env(*names: str) -> str:
    for name in names:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return ""


def load_dotenv(path: str) -> None:
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8") as fp:
        for raw_line in fp:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip("'\"")
            if key and key not in os.environ:
                os.environ[key] = value


def percent_encode(value: str) -> str:
    return urllib.parse.quote(value, safe="-_.~")


def create_aliyun_nls_token(access_key_id: str, access_key_secret: str) -> tuple[str, int]:
    timestamp = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    params = {
        "AccessKeyId": access_key_id,
        "Action": "CreateToken",
        "Format": "JSON",
        "RegionId": TOKEN_REGION_ID,
        "SignatureMethod": "HMAC-SHA1",
        "SignatureNonce": str(uuid.uuid4()),
        "SignatureVersion": "1.0",
        "Timestamp": timestamp,
        "Version": TOKEN_API_VERSION,
    }
    canonical_query = "&".join(
        f"{percent_encode(key)}={percent_encode(params[key])}" for key in sorted(params)
    )
    string_to_sign = "GET&%2F&" + percent_encode(canonical_query)
    digest = hmac.new(
        (access_key_secret + "&").encode("utf-8"),
        string_to_sign.encode("utf-8"),
        hashlib.sha1,
    ).digest()
    signature = base64.b64encode(digest).decode("ascii")
    query = "Signature=" + percent_encode(signature) + "&" + canonical_query
    url = TOKEN_META_ENDPOINT + "?" + query
    try:
        with urllib.request.urlopen(url, timeout=15) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Aliyun CreateToken HTTP {exc.code}: {detail}") from exc

    token = payload.get("Token", {})
    token_id = token.get("Id", "")
    expire_time = int(token.get("ExpireTime", 0) or 0)
    if not token_id or not expire_time:
        raise RuntimeError(f"Aliyun CreateToken returned no token: {payload}")
    return token_id, expire_time


def first_value(query: dict, key: str) -> str:
    value = query.get(key, [""])
    if isinstance(value, list):
        return value[0] if value else ""
    return str(value)


def parse_bool(value: str) -> bool:
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def aliyun_tts_pcm_for_server(server: AliyunVoiceServer, text: str) -> bytes:
    started = time.perf_counter()
    params = {
        "appkey": server.appkey,
        "token": server.get_token(),
        "text": text,
        "format": "pcm",
        "sample_rate": server.sample_rate,
        "voice": server.voice,
        "volume": server.volume,
        "speech_rate": server.speech_rate,
        "pitch_rate": server.pitch_rate,
    }
    url = server.tts_url + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=server.tts_request_timeout) as resp:
            content_type = resp.headers.get("Content-Type", "")
            if "json" in content_type:
                raise RuntimeError(resp.read().decode("utf-8", errors="replace"))
            audio = resp.read()
            elapsed_ms = (time.perf_counter() - started) * 1000
            log_print(f"阿里云 TTS 成功: chars={len(text)} bytes={len(audio)} elapsed_ms={elapsed_ms:.0f}")
            return audio
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Aliyun TTS HTTP {exc.code}: {detail}") from exc


def aliyun_tts_pcm_with_retries_for_server(server: AliyunVoiceServer, text: str) -> bytes:
    last_error: Exception | None = None
    for attempt in range(1, server.tts_retries + 2):
        try:
            return aliyun_tts_pcm_for_server(server, text)
        except Exception as exc:
            last_error = exc
            log_print(f"TTS 第 {attempt} 次尝试失败: text={text!r} error={exc}", file=sys.stderr)
    raise RuntimeError(f"Aliyun TTS failed after {server.tts_retries + 1} attempt(s): {last_error}")


def event_audio_cache_meta(server: AliyunVoiceServer, text: str) -> dict:
    return {
        "version": EVENT_AUDIO_CACHE_META_VERSION,
        "text": str(text or ""),
        "format": "pcm_s16le",
        "sample_rate": int(getattr(server, "sample_rate", 0) or 0),
        "voice": str(getattr(server, "voice", "") or ""),
        "volume": int(getattr(server, "volume", 0) or 0),
        "speech_rate": int(getattr(server, "speech_rate", 0) or 0),
        "pitch_rate": int(getattr(server, "pitch_rate", 0) or 0),
        "tts_url": str(getattr(server, "tts_url", "") or ""),
        "appkey": str(getattr(server, "appkey", "") or ""),
    }


def read_event_audio_cache_meta(meta_path: str) -> dict:
    if not os.path.exists(meta_path):
        return {}
    try:
        raw = read_binary_file(meta_path).decode("utf-8")
    except UnicodeDecodeError:
        return {}
    try:
        loaded = json.loads(raw)
    except json.JSONDecodeError:
        return {"text": raw}
    return loaded if isinstance(loaded, dict) else {}


def write_event_audio_cache_meta(meta_path: str, meta: dict) -> None:
    tmp_meta_path = f"{meta_path}.tmp"
    with open(tmp_meta_path, "w", encoding="utf-8") as fp:
        fp.write(json.dumps(meta, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    os.replace(tmp_meta_path, meta_path)


def ensure_event_audio_cache(server: AliyunVoiceServer, name: str, *, logger=log_print) -> tuple[str, str]:
    if name not in EVENT_AUDIO_TEXT:
        raise ValueError(f"unknown event audio: {name}")
    cache_dir = os.path.join(server.static_dir, "event-audio")
    os.makedirs(cache_dir, exist_ok=True)
    pcm_path = os.path.join(cache_dir, f"{name}.pcm")
    wav_path = os.path.join(cache_dir, f"{name}.wav")
    meta_path = os.path.join(cache_dir, f"{name}.txt")
    text = EVENT_AUDIO_TEXT[name]
    expected_meta = event_audio_cache_meta(server, text)
    cached_meta = read_event_audio_cache_meta(meta_path)
    if cached_meta != expected_meta:
        for stale_path in (pcm_path, wav_path):
            try:
                os.remove(stale_path)
            except FileNotFoundError:
                pass
        if cached_meta:
            previous_text = str(cached_meta.get("text") or "")
            if previous_text and previous_text != text:
                logger(f"Event audio text changed: {name} {previous_text!r} -> {text!r}")
            else:
                logger(f"Event audio TTS config changed: {name}")
    if not os.path.exists(pcm_path) or os.path.getsize(pcm_path) == 0:
        logger(f"Event audio cache miss: {name} -> {text!r}")
        audio = aliyun_tts_pcm_with_retries_for_server(server, text)
        tmp_path = f"{pcm_path}.tmp"
        with open(tmp_path, "wb") as fp:
            fp.write(audio)
        os.replace(tmp_path, pcm_path)
        write_event_audio_cache_meta(meta_path, expected_meta)
        logger(f"Event audio cached: {pcm_path} bytes={len(audio)}")
    if not os.path.exists(wav_path) or os.path.getsize(wav_path) == 0:
        pcm = read_binary_file(pcm_path)
        tmp_path = f"{wav_path}.tmp"
        with open(tmp_path, "wb") as fp:
            fp.write(pcm_to_wav(pcm, server.sample_rate))
        os.replace(tmp_path, wav_path)
    return pcm_path, wav_path


def prewarm_event_audio_cache(server: AliyunVoiceServer, names: tuple[str, ...] | None = None) -> None:
    selected_names = names or tuple(EVENT_AUDIO_TEXT)
    for name in selected_names:
        try:
            ensure_event_audio_cache(server, name)
        except Exception as exc:
            log_print(f"事件音频预热失败: name={name} error={exc}", file=sys.stderr)


def safe_device_id(device_id: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.:-]+", "_", str(device_id).strip())[:64]
    return safe or "default"


def is_placeholder_device_id(device_id: str) -> bool:
    value = str(device_id).strip().upper()
    return value in ("", "DEFAULT", "AA:BB:CC:DD:EE:FF", "AABBCCDDEEFF")


def normalize_expression_name(expression: str) -> str:
    value = str(expression or "").strip().lower()
    if not value:
        return "calm"
    return EXPRESSION_ALIASES.get(value, value)


DEVICE_STATE_ALIASES = {
    "wait": "waiting",
    "think": "waiting",
    "thinking": "waiting",
    "awake": "listening",
    "wake": "listening",
    "listen": "listening",
    "sleep": "idle",
    "sleeping": "idle",
}


def normalize_device_state_name(state: str) -> str:
    value = str(state or "").strip().lower()
    if not value:
        return "waiting"
    return DEVICE_STATE_ALIASES.get(value, value)


def first_connected_device_id(
    last_seen: dict[str, float],
    device_order: list[str],
    now: float | None = None,
) -> str:
    if not last_seen:
        return "default"
    now = time.time() if now is None else now
    for device_id in device_order:
        seen = last_seen.get(device_id)
        if seen is not None and now - seen <= DEVICE_ONLINE_TTL_SECONDS:
            return device_id
    return "default"


def http_device_online(server, device_id: str, now: float | None = None) -> bool:
    device_id = safe_device_id(device_id)
    if is_placeholder_device_id(device_id):
        return False
    now = time.time() if now is None else now
    with server.device_lock:
        seen = server.last_seen.get(device_id)
    return seen is not None and now - seen <= DEVICE_ONLINE_TTL_SECONDS


def connected_device_ids(server) -> list[str]:
    now = time.time()
    device_ids: list[str] = []
    seen_ids = set()

    manager = getattr(server, "realtime_manager", None)
    if manager:
        for device in manager.devices_snapshot():
            device_id = safe_device_id(device.get("device_id", ""))
            if device_id and device_id not in seen_ids and not is_placeholder_device_id(device_id):
                device_ids.append(device_id)
                seen_ids.add(device_id)

    with server.device_lock:
        last_seen_snapshot = dict(server.last_seen)
        ordered_ids = list(server.device_order)

    for device_id in ordered_ids + list(last_seen_snapshot):
        device_id = safe_device_id(device_id)
        if not device_id or device_id in seen_ids or is_placeholder_device_id(device_id):
            continue
        seen = last_seen_snapshot.get(device_id)
        if seen is not None and now - seen <= DEVICE_ONLINE_TTL_SECONDS:
            device_ids.append(device_id)
            seen_ids.add(device_id)

    return device_ids


def enqueue_server_command(server, device_id: str, command: dict, *, persist: bool = True) -> bool:
    device_id = safe_device_id(device_id)
    if is_placeholder_device_id(device_id):
        return False

    command_type = str(command.get("type") or "")
    payload = command.get("payload")
    apply_current_speech_generation(server, device_id, command_type, payload)
    prepare_server_command_audio(server, command_type, payload)
    normalize_command_speech_payload(command_type, payload)
    command_store = getattr(server, "command_store", None)
    if persist and command_store is not None:
        try:
            command["boot_id"] = command_store.current_boot_id(device_id)
            command_store.create_command(CommandEnvelope.from_legacy(device_id, command))
        except Exception as exc:
            log_print(
                f"周期命令持久化失败: device={device_id} cmd_id={command.get('cmd_id', '')} error={exc}",
                file=sys.stderr,
            )
            return False
    with server.device_lock:
        queue = server.device_queues.get(device_id)
        if queue is None:
            queue = DeviceCommandQueue(getattr(server, "command_queue_max_size", COMMAND_QUEUE_MAX_SIZE))
            server.device_queues[device_id] = queue
    stats = queue.put(command)
    if stats.get("queued"):
        log_print(f"周期命令已入队: type={command.get('type')} device={device_id}")
        return True
    log_print(f"周期命令已丢弃: type={command.get('type')} device={device_id} stats={stats}")
    return False


def enqueue_sedentary_reminder_once(server, reminder_index: int = 0, *, trigger_id: str = "") -> int:
    """Ask each online device to face its owner and speak only if a face is found."""
    device_ids = connected_device_ids(server)
    if not device_ids:
        return 0

    _name, text = SEDENTARY_REMINDER_EVENTS[reminder_index % len(SEDENTARY_REMINDER_EVENTS)]
    trigger_id = trigger_id or f"sedentary:{int(time.time())}"
    queued = 0
    for device_id in device_ids:
        command = make_command(
            "find_owner",
            {
                "rounds": 1,
                "reply": text,
                "trigger_id": trigger_id,
                "speak": True,
                "preserve_speech": True,
                "wait_for_speech": True,
                "gain_x": float(getattr(server, "find_owner_gain_x", 1.0)),
                "gain_y": float(getattr(server, "find_owner_gain_y", 0.8)),
                "stop_pixels": float(getattr(server, "find_owner_stop_pixels", 32.0)),
            },
            priority=COMMAND_DEFAULT_PRIORITIES["find_owner"],
            interrupt=False,
            ttl_seconds=300,
            discardable=True,
            coalesce_key="sedentary_timer",
        )
        command["source_type"] = "sedentary_timer"
        command["source_id"] = device_id
        command["segment_index"] = time.time_ns()
        command_store = getattr(server, "command_store", None)
        if command_store is not None:
            command_store.cancel_pending_by_source(
                "sedentary_timer",
                device_id,
                "superseded by newer sedentary timer",
            )
        if enqueue_server_command(server, device_id, command):
            queued += 1

    server.sedentary_reminder_queued_total = int(
        getattr(server, "sedentary_reminder_queued_total", 0)
    ) + queued
    return queued


def run_periodic_sedentary_reminder_loop(server) -> None:
    interval = int(
        getattr(
            server,
            "sedentary_reminder_interval_seconds",
            DEFAULT_SEDENTARY_REMINDER_INTERVAL_SECONDS,
        )
        or 0
    )
    if interval <= 0:
        log_print("定时久坐提醒已禁用")
        return

    log_print(f"定时久坐提醒已启用: 间隔={interval}s，播报前需检测到人脸")
    next_due = time.monotonic() + interval
    reminder_index = 0
    while True:
        remaining = next_due - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
        now = time.monotonic()
        next_due += interval
        if next_due <= now:
            # Do not burst missed reminders after a long process stall.
            next_due = now + interval

        server.sedentary_reminder_trigger_total = int(
            getattr(server, "sedentary_reminder_trigger_total", 0)
        ) + 1
        trigger_id = f"sedentary:{int(time.time())}"
        queued = enqueue_sedentary_reminder_once(server, reminder_index, trigger_id=trigger_id)
        if queued:
            reminder_index += 1
            log_print(f"定时久坐找人命令已入队: trigger={trigger_id} devices={queued}")
        else:
            log_print(f"定时久坐提醒跳过: trigger={trigger_id} 无在线设备")


def run_periodic_ota_check_loop(server) -> None:
    interval = int(getattr(server, "ota_check_interval_seconds", DEFAULT_OTA_CHECK_INTERVAL_SECONDS) or 0)
    if interval <= 0:
        log_print("周期 OTA 检查命令已禁用")
        return

    log_print(f"周期 OTA 检查命令已启用: 间隔={interval}s")
    while True:
        time.sleep(interval)
        firmware = find_latest_ota_firmware(server.ota_firmware_file, server.ota_firmware_dir)
        if firmware is None:
            continue
        device_ids = connected_device_ids(server)
        if not device_ids:
            continue
        for device_id in device_ids:
            command = make_command(
                "check_ota",
                {},
                priority=25,
                interrupt=False,
                ttl_seconds=max(interval * 2, 600),
                discardable=True,
                coalesce_key="check_ota",
            )
            enqueue_server_command(server, device_id, command)


def make_command(
    command_type: str,
    payload,
    priority: int = 0,
    interrupt: bool = False,
    ttl_seconds: float | None = None,
    discardable: bool | None = None,
    coalesce_key: str = "",
) -> dict:
    normalized_type = str(command_type or "")
    effective_priority = max(int(priority or 0), command_default_priority(normalized_type))
    if ttl_seconds is None:
        ttl_seconds = command_default_ttl(normalized_type)
    if discardable is None:
        discardable = normalized_type in COMMAND_DISCARDABLE_TYPES
    if not coalesce_key and normalized_type in COMMAND_COALESCE_BY_TYPE:
        coalesce_key = normalized_type
    return {
        "cmd_id": f"cmd_{uuid.uuid4().hex[:12]}",
        "type": normalized_type,
        "priority": effective_priority,
        "interrupt": bool(interrupt or normalized_type == "stop"),
        "ttl_seconds": ttl_seconds,
        "discardable": bool(discardable),
        "coalesce_key": coalesce_key,
        "payload": payload,
        "created_at": time.time(),
    }


def save_morrow_notice(server, notice: dict) -> bool:
    notice_id = notice.get("id")
    if not notice_id:
        return False
    with server.v3_database.connect() as conn:
        row = conn.execute("SELECT state FROM morrow_notices WHERE notice_id=?", (notice_id,)).fetchone()
        if row is not None:
            # Already exists, de-duplicate!
            return False

        kind = notice.get("kind", "unknown") or "unknown"
        text = notice.get("text", "") or ""
        timestamp_ms = int(notice.get("timestamp_ms") or 0)
        
        # Priority and TTL mapping
        if kind == "meeting_reminder":
            ttl_seconds = 600
        elif kind == "fieldwork_reminder":
            ttl_seconds = 1800
        elif kind == "travel_reminder":
            ttl_seconds = 21600
        else:
            ttl_seconds = 1800
            
        now = _dt.datetime.now(_dt.timezone.utc)
        expires_at = (now + _dt.timedelta(seconds=ttl_seconds)).isoformat()
        received_at = now.isoformat()
        
        conn.execute(
            """
            INSERT INTO morrow_notices (
              notice_id, kind, timestamp_ms, text, state, expires_at, received_at
            ) VALUES (?, ?, ?, ?, 'received', ?, ?)
            """,
            (notice_id, kind, timestamp_ms, text, expires_at, received_at),
        )
        return True


def mark_morrow_notice_state(server, notice_id: str, state: str, message: str = "") -> None:
    now = _dt.datetime.now(_dt.timezone.utc).isoformat()
    with server.v3_database.connect() as conn:
        conn.execute(
            "UPDATE morrow_notices SET state=?, updated_at=?, last_message=? WHERE notice_id=?",
            (state, now, message, notice_id),
        )


def device_has_pending_dialogue(server, device_id: str) -> bool:
    """Keep notices behind the complete dialogue, including device playback."""
    coordinator = getattr(server, "morrow_coordinator", None)
    has_pending_turn = getattr(coordinator, "has_pending_turn", None)
    if callable(has_pending_turn) and has_pending_turn(device_id):
        return True
    command_store = getattr(server, "command_store", None)
    return bool(command_store and command_store.has_unfinished_dialogue(device_id))


def submit_morrow_notice_text(server, notice_id: str) -> bool:
    with server.v3_database.connect() as conn:
        row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id=?", (notice_id,)).fetchone()
        if not row:
            return False
        kind = row["kind"]
        text = row["text"]
        expires_at = row["expires_at"]
        state = row["state"]
        
    now_dt = _dt.datetime.now(_dt.timezone.utc)
    expires_dt = _dt.datetime.fromisoformat(expires_at)
    if now_dt > expires_dt:
        mark_morrow_notice_state(server, notice_id, "expired", "expired before queuing")
        return False
        
    device_ids = connected_device_ids(server)
    if not device_ids:
        return False
        
    device_id = device_ids[0]
    if device_has_pending_dialogue(server, device_id):
        return False

    coordinator = getattr(server, "morrow_coordinator", None)
    generation_getter = getattr(coordinator, "generation_for_device", None)
    generation = int(generation_getter(device_id)) if callable(generation_getter) else 0
    prio = DIALOGUE_COMMAND_PRIORITY
        
    remaining_ttl = int(max(10, (expires_dt - now_dt).total_seconds()))
    stream_id = notice_id
    queued_count = 0
    segment_index = 0
    last_cmd_id = ""

    cleaned_text, expression = parse_expression_tags(text)
    speech_segments = []
    for raw_segment in split_sentences(cleaned_text, int(getattr(server, "max_sentence_chars", 120) or 120)):
        segment = normalize_speech_text_for_voice(str(raw_segment or ""))
        if not segment or speech_text_is_temporarily_suppressed(segment):
            continue
        speech_segments.append(segment)

    for offset, segment in enumerate(speech_segments):
        segment_index += 1
        command = make_command(
            "speak",
            {
                "text": segment,
                "expression": expression,
                "turn_id": stream_id,
                "segment_index": offset,
                "generation": generation,
                "reply_end": offset == len(speech_segments) - 1,
                "pause_listener": True,
            },
            priority=prio,
            interrupt=False,
            ttl_seconds=remaining_ttl,
            discardable=False,
            coalesce_key=f"{stream_id}:{segment_index}",
        )
        command["source_type"] = "morrow_notice"
        command["source_id"] = notice_id
        command["segment_index"] = segment_index
        command["turn_generation"] = generation
        last_cmd_id = command.get("cmd_id", "")
        if enqueue_server_command(server, device_id, command):
            queued_count += 1
            
    if queued_count > 0:
        log_print(f"Morrow 主动提醒已入队: device={device_id} segments={queued_count} last_cmd={last_cmd_id}")
        now_str = _dt.datetime.now(_dt.timezone.utc).isoformat()
        with server.v3_database.connect() as conn:
            conn.execute(
                """
                UPDATE morrow_notices
                   SET state='queued', command_id=?, updated_at=?, last_message=?
                 WHERE notice_id=?
                """,
                (last_cmd_id, now_str, f"queued to {device_id}", notice_id),
            )
        return True
    return False


def run_morrow_notice_outbox_loop(server) -> None:
    while True:
        time.sleep(5)
        if not connected_device_ids(server):
            continue
        now_dt = _dt.datetime.now(_dt.timezone.utc)
        now_str = now_dt.isoformat()
        
        # 1. Update expired ones that are still in received state
        with server.v3_database.connect() as conn:
            conn.execute(
                """
                UPDATE morrow_notices
                   SET state='expired', last_error='expired before queuing'
                 WHERE state='received' AND expires_at < ?
                """,
                (now_str,),
            )
            
            # 2. Get non-expired received notices to submit
            rows = conn.execute(
                """
                SELECT notice_id FROM morrow_notices
                 WHERE state='received' AND expires_at >= ?
                   AND (command_id IS NULL OR command_id='')
                 ORDER BY received_at ASC
                 LIMIT 8
                """,
                (now_str,),
            ).fetchall()
            
        for row in rows:
            submit_morrow_notice_text(server, row["notice_id"])


def run_morrow_notice_listener(server) -> None:
    client = getattr(server, "morrow_client", None)
    if client is None:
        return

    stop_event = server.morrow_notice_stop_event
    backoff_seconds = 0.5
    
    log_print("Morrow 主动提醒监听线程已启动，正在消费 client.notices Queue...")
    while not stop_event.is_set():
        try:
            try:
                notice = client.notices.get(timeout=1.0)
            except Empty:
                continue
                
            log_print(f"收到 Morrow 主动提醒 WebSocket 消息: {notice}")
            
            # Save and de-duplicate
            if save_morrow_notice(server, notice):
                notice_id = notice.get("id")
                # Attempt immediate submission if device is online
                submit_morrow_notice_text(server, notice_id)
                
            backoff_seconds = 0.5
        except Exception as exc:
            if stop_event.is_set():
                break
            log_print(f"Morrow 主动提醒监听线程异常: {exc}", file=sys.stderr)
            stop_event.wait(backoff_seconds)
            backoff_seconds = min(backoff_seconds * 2, 30.0)


def command_payload_from_query(command_type: str, query: dict):
    if command_type in ("state", "device_state"):
        return {
            "state": normalize_device_state_name(first_value(query, "state") or first_value(query, "name") or "waiting")
        }
    if command_type in ("face", "expression", "action"):
        expression = first_value(query, "expression") or first_value(query, "face") or "calm"
        if command_type in ("expression", "action"):
            expression = first_value(query, "name") or first_value(query, "action") or expression
        return {"expression": normalize_expression_name(expression)}
    if command_type == "speak":
        payload = {"text": first_value(query, "text") or "你好呀"}
        expression = first_value(query, "expression")
        if expression:
            payload["expression"] = normalize_expression_name(expression)
        reply_end = first_value(query, "reply_end")
        if reply_end:
            payload["reply_end"] = parse_bool(reply_end)
        voice = first_value(query, "voice")
        if voice:
            payload["voice"] = voice
        for key in ("sample_rate", "volume", "speech_rate", "pitch_rate"):
            value = first_value(query, key)
            if value != "":
                payload[key] = int(value)
        return payload
    if command_type in ("volume", "sound"):
        direction = first_value(query, "direction") or first_value(query, "action") or first_value(query, "type") or "up"
        mode = first_value(query, "mode") or ""
        value = first_value(query, "value")
        if mode == "set" or value:
            return {
                "mode": "set",
                "value": int(value or "100"),
            }
        return {
            "direction": direction,
            "step": int(first_value(query, "step") or "10"),
        }
    if command_type == "play_audio":
        return {"url": first_value(query, "url")}
    if command_type in ("motion", "move"):
        motion_type = first_value(query, "type") or first_value(query, "action") or first_value(query, "direction")
        if motion_type:
            return {
                "type": motion_type,
                "degree": float(first_value(query, "degree") or first_value(query, "degrees") or "15"),
                "duration_ms": int(first_value(query, "duration_ms") or "500"),
            }
        return {
            "pan": float(first_value(query, "pan") or "0"),
            "tilt": float(first_value(query, "tilt") or "45"),
            "duration_ms": int(first_value(query, "duration_ms") or "500"),
        }
    if command_type in ("find_owner", "locate_owner"):
        speak_raw = first_value(query, "speak")
        speak = parse_bool(speak_raw) if speak_raw else True
        return {
            "rounds": int(first_value(query, "rounds") or "1"),
            "reply": first_value(query, "reply") or ("我在" if speak else ""),
            "speak": speak,
            "preserve_speech": parse_bool(first_value(query, "preserve_speech") or "false"),
            "wait_for_speech": parse_bool(first_value(query, "wait_for_speech") or "false"),
            "gain_x": float(first_value(query, "gain_x") or "1.0"),
            "gain_y": float(first_value(query, "gain_y") or "0.8"),
            "stop_pixels": float(first_value(query, "stop_pixels") or "32"),
        }
    if command_type in ("check_ota", "ota_check", "firmware_ota"):
        return {}
    if command_type == "stop":
        return {}
    if command_type == "sequence":
        raw = first_value(query, "payload") or first_value(query, "steps")
        if raw:
            try:
                payload = json.loads(raw)
                if isinstance(payload, list):
                    return payload
            except json.JSONDecodeError:
                pass
        text = first_value(query, "text")
        expression = normalize_expression_name(first_value(query, "expression") or "calm")
        face_step = {"type": "face", "expression": expression}
        steps = [face_step]
        if text:
            speak_step = {"type": "speak", "text": text, "pause_listener": True}
            voice = first_value(query, "voice")
            if voice:
                speak_step["voice"] = voice
            for key in ("sample_rate", "volume", "speech_rate", "pitch_rate"):
                value = first_value(query, key)
                if value != "":
                    speak_step[key] = int(value)
            steps.append(speak_step)
        return steps
    return {key: values[0] for key, values in query.items() if values}


def rgb565_to_bmp(rgb565: bytes, width: int, height: int) -> bytes:
    row_stride = width * 3
    padding = (4 - (row_stride % 4)) % 4
    pixel_bytes = (row_stride + padding) * height
    file_size = 14 + 40 + pixel_bytes

    out = bytearray()
    out += b"BM"
    out += struct.pack("<IHHI", file_size, 0, 0, 54)
    out += struct.pack("<IIIHHIIIIII", 40, width, height, 1, 24, 0, pixel_bytes, 2835, 2835, 0, 0)

    for y in range(height - 1, -1, -1):
        row_start = y * width * 2
        for x in range(width):
            hi = rgb565[row_start + x * 2]
            lo = rgb565[row_start + x * 2 + 1]
            value = (hi << 8) | lo
            r = ((value >> 11) & 0x1F) * 255 // 31
            g = ((value >> 5) & 0x3F) * 255 // 63
            b = (value & 0x1F) * 255 // 31
            out += bytes((b, g, r))
        out += b"\x00" * padding
    return bytes(out)


def rgb565_to_rgb_rows(rgb565: bytes, width: int, height: int) -> list[bytes]:
    rows = []
    for y in range(height):
        row_start = y * width * 2
        row = bytearray(width * 3)
        out = 0
        for x in range(width):
            hi = rgb565[row_start + x * 2]
            lo = rgb565[row_start + x * 2 + 1]
            value = (hi << 8) | lo
            row[out] = ((value >> 11) & 0x1F) * 255 // 31
            row[out + 1] = ((value >> 5) & 0x3F) * 255 // 63
            row[out + 2] = (value & 0x1F) * 255 // 31
            out += 3
        rows.append(bytes(row))
    return rows


def yuv_to_rgb_pixel(y: int, u: int, v: int) -> tuple[int, int, int]:
    c = y - 16
    d = u - 128
    e = v - 128
    r = (298 * c + 409 * e + 128) >> 8
    g = (298 * c - 100 * d - 208 * e + 128) >> 8
    b = (298 * c + 516 * d + 128) >> 8
    return max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b))


def yuv422_to_rgb_rows(yuv422: bytes, width: int, height: int) -> list[bytes]:
    rows = []
    for y in range(height):
        row_start = y * width * 2
        row = bytearray(width * 3)
        out = 0
        for x in range(0, width, 2):
            offset = row_start + x * 2
            y0 = yuv422[offset]
            u = yuv422[offset + 1]
            y1 = yuv422[offset + 2]
            v = yuv422[offset + 3]
            r, g, b = yuv_to_rgb_pixel(y0, u, v)
            row[out] = r
            row[out + 1] = g
            row[out + 2] = b
            r, g, b = yuv_to_rgb_pixel(y1, u, v)
            row[out + 3] = r
            row[out + 4] = g
            row[out + 5] = b
            out += 6
        rows.append(bytes(row))
    return rows


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)


def rgb565_to_png(rgb565: bytes, width: int, height: int) -> bytes:
    raw = b"".join(b"\x00" + row for row in rgb565_to_rgb_rows(rgb565, width, height))
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    out += png_chunk(b"IDAT", zlib.compress(raw, level=6))
    out += png_chunk(b"IEND", b"")
    return bytes(out)


def yuv422_to_png(yuv422: bytes, width: int, height: int) -> bytes:
    raw = b"".join(b"\x00" + row for row in yuv422_to_rgb_rows(yuv422, width, height))
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    out += png_chunk(b"IDAT", zlib.compress(raw, level=6))
    out += png_chunk(b"IEND", b"")
    return bytes(out)


def detect_and_visualize_faces(image_path: str, output_path: str) -> tuple[str, dict]:
    try:
        import face_recognition
        from PIL import Image, ImageDraw, ImageFont
    except Exception as exc:
        return "", {"available": False, "error": f"face_recognition/Pillow unavailable: {exc}", "faces": []}

    try:
        image = face_recognition.load_image_file(image_path)
        locations = face_recognition.face_locations(image, number_of_times_to_upsample=1, model="hog")
        landmarks = face_recognition.face_landmarks(image, locations)

        pil_image = Image.open(image_path).convert("RGB")
        draw = ImageDraw.Draw(pil_image)
        try:
            font = ImageFont.truetype("DejaVuSans.ttf", 14)
        except Exception:
            font = ImageFont.load_default()

        faces = []
        for idx, (top, right, bottom, left) in enumerate(locations, start=1):
            faces.append(
                {
                    "top": top,
                    "right": right,
                    "bottom": bottom,
                    "left": left,
                    "center": {"x": (left + right) / 2, "y": (top + bottom) / 2},
                    "area": (right - left) * (bottom - top),
                }
            )
            draw.rectangle(((left, top), (right, bottom)), outline=(0, 255, 0), width=3)
            label = f"face {idx}"
            text_box = draw.textbbox((left, top), label, font=font)
            label_h = text_box[3] - text_box[1] + 4
            draw.rectangle(((left, max(0, top - label_h)), (left + text_box[2] - text_box[0] + 8, top)), fill=(0, 160, 0))
            draw.text((left + 4, max(0, top - label_h + 2)), label, fill=(255, 255, 255), font=font)

        for face_landmarks in landmarks:
            for points in face_landmarks.values():
                if len(points) > 1:
                    draw.line(points, fill=(255, 220, 0), width=2)

        pil_image.save(output_path, "PNG")
        best_face = max(faces, key=lambda face: face["area"], default=None)
        return output_path, {"available": True, "faces": faces, "best_face": best_face, "landmarks": landmarks}
    except Exception as exc:
        return "", {"available": True, "error": str(exc), "faces": []}


def main():
    load_dotenv(os.path.join(os.path.dirname(os.path.dirname(__file__)), ".env"))
    save_recording_default = parse_bool(
        os.environ.get("STACKCHAN_SAVE_RECORDING", os.environ.get("STACKCHAN_SAVE_AUDIO_UPLOADS", "true"))
    )

    parser = argparse.ArgumentParser(description="Local Xiaopai bridge for Aliyun ASR and PCM streaming TTS.")
    parser.add_argument("--debug", action="store_true", default=parse_bool(os.environ.get("STACKCHAN_DEBUG", "false")))
    parser.add_argument("--host", default=os.environ.get("STACKCHAN_ALIYUN_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_PORT", "8091")))
    parser.add_argument("--region", choices=sorted(ASR_URLS), default=os.environ.get("STACKCHAN_ALIYUN_REGION", "shanghai"))
    parser.add_argument("--tts-url", default=os.environ.get("STACKCHAN_ALIYUN_TTS_URL", ""))
    parser.add_argument("--voice", default=os.environ.get("STACKCHAN_ALIYUN_VOICE", DEFAULT_TTS_VOICE))
    parser.add_argument(
        "--asr-sample-rate",
        type=int,
        default=int(os.environ.get("STACKCHAN_ALIYUN_ASR_SAMPLE_RATE", "16000")),
    )
    parser.add_argument("--sample-rate", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_SAMPLE_RATE", "24000")))
    parser.add_argument("--volume", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_VOLUME", "80")))
    parser.add_argument(
        "--speaker-volume",
        type=int,
        default=int(os.environ.get("STACKCHAN_SPEAKER_VOLUME", str(SPEAKER_VOLUME_DEFAULT))),
        help="Global Xiaopai hardware speaker volume percent attached to every speech command.",
    )
    parser.add_argument("--speech-rate", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_SPEECH_RATE", "0")))
    parser.add_argument("--pitch-rate", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_PITCH_RATE", "0")))
    parser.add_argument("--max-sentence-chars", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_MAX_SENTENCE_CHARS", "120")))
    parser.add_argument("--chunk-size", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_CHUNK_SIZE", "4096")))
    parser.add_argument("--tts-prefetch-workers", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_TTS_PREFETCH_WORKERS", "2")))
    parser.add_argument("--tts-request-timeout", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_TTS_REQUEST_TIMEOUT", "12")))
    parser.add_argument("--tts-retries", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_TTS_RETRIES", "2")))
    parser.add_argument("--tts-tail-silence-ms", type=int, default=int(os.environ.get("STACKCHAN_TTS_TAIL_SILENCE_MS", "0")))
    parser.add_argument("--command-queue-max-size", type=int, default=int(os.environ.get("STACKCHAN_COMMAND_QUEUE_MAX_SIZE", str(COMMAND_QUEUE_MAX_SIZE))))
    parser.add_argument("--capture-dir", default=os.environ.get("STACKCHAN_CAPTURE_DIR", "captures"))
    parser.add_argument(
        "--capture-save-mode",
        choices=("none", "raw", "debug"),
        default=os.environ.get("STACKCHAN_CAPTURE_SAVE_MODE", "none"),
        help="Image upload persistence: none, raw, or debug (raw + converted images + face visualizations).",
    )
    parser.add_argument(
        "--save-recording",
        dest="save_recording",
        action=argparse.BooleanOptionalAction,
        default=save_recording_default,
        help="Save Xiaopai listened audio under <capture-dir>/audio for debugging.",
    )
    parser.add_argument(
        "--save-audio-uploads",
        dest="save_recording",
        action=argparse.BooleanOptionalAction,
        default=argparse.SUPPRESS,
        help="Alias for --save-recording.",
    )
    parser.add_argument(
        "--wifi-logs",
        action=argparse.BooleanOptionalAction,
        default=parse_bool(os.environ.get("STACKCHAN_WIFI_LOGS", "true")),
        help="Accept Wi-Fi log uploads from Xiaopai firmware.",
    )
    parser.add_argument(
        "--usb-serial",
        action=argparse.BooleanOptionalAction,
        default=parse_bool(os.environ.get("STACKCHAN_USB_SERIAL", "true")),
        help="Tell firmware whether to keep local USB/serial console output enabled.",
    )
    parser.add_argument(
        "--state-events",
        action=argparse.BooleanOptionalAction,
        default=parse_bool(os.environ.get("STACKCHAN_STATE_EVENTS", "true")),
        help="Tell firmware whether to upload xiaopai-state/expression-state events.",
    )
    parser.add_argument(
        "--device-config-poll-ms",
        type=int,
        default=int(os.environ.get("STACKCHAN_DEVICE_CONFIG_POLL_MS", "5000")),
    )
    parser.add_argument(
        "--device-log-post-interval-ms",
        type=int,
        default=int(os.environ.get("STACKCHAN_DEVICE_LOG_POST_INTERVAL_MS", "1000")),
    )
    parser.add_argument(
        "--device-log-dir",
        default=os.environ.get("STACKCHAN_DEVICE_LOG_DIR", ""),
        help="Directory for readable per-device log files. Defaults to <capture-dir>/device-logs.",
    )
    parser.add_argument("--static-dir", default=os.environ.get("STACKCHAN_STATIC_DIR", "static"))
    parser.add_argument(
        "--database-path",
        default=os.environ.get("STACKCHAN_DATABASE_PATH", os.path.join("data", "xiaopai-v3.sqlite3")),
        help="SQLite WAL database for V3 devices, commands, ACKs and deliveries.",
    )
    parser.add_argument(
        "--face-detector",
        choices=("yunet", "legacy", "none"),
        default=os.environ.get("STACKCHAN_FACE_DETECTOR", "yunet"),
        help="Face detection backend for /upload-image.",
    )
    parser.add_argument(
        "--yunet-model",
        default=os.environ.get(
            "STACKCHAN_YUNET_MODEL",
            os.path.join(os.path.dirname(os.path.dirname(__file__)), "models", "face_detection_yunet_2023mar.onnx"),
        ),
    )
    parser.add_argument(
        "--yunet-score-threshold",
        type=float,
        default=float(os.environ.get("STACKCHAN_YUNET_SCORE_THRESHOLD", "0.45")),
    )
    parser.add_argument(
        "--yunet-nms-threshold",
        type=float,
        default=float(os.environ.get("STACKCHAN_YUNET_NMS_THRESHOLD", "0.3")),
    )
    parser.add_argument("--yunet-top-k", type=int, default=int(os.environ.get("STACKCHAN_YUNET_TOP_K", "5000")))
    parser.add_argument(
        "--visual-tracking-enabled",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Queue Xiaopai motion commands from /upload-image face detections.",
    )
    parser.add_argument(
        "--visual-tracking-deadzone-px",
        type=float,
        default=20.0,
    )
    parser.add_argument(
        "--visual-tracking-gain-x",
        type=float,
        default=1.0,
        help="Horizontal multiplier for pixel error to head-motion degree conversion.",
    )
    parser.add_argument(
        "--visual-tracking-gain-y",
        type=float,
        default=1.0,
        help="Vertical multiplier for pixel error to head-motion degree conversion.",
    )
    parser.add_argument(
        "--visual-tracking-max-degree",
        type=float,
        default=12.0,
    )
    parser.add_argument(
        "--visual-tracking-min-degree",
        type=float,
        default=2.0,
    )
    parser.add_argument(
        "--visual-tracking-duration-ms",
        type=int,
        default=280,
    )
    parser.add_argument(
        "--visual-tracking-min-interval-ms",
        type=int,
        default=350,
    )
    parser.add_argument(
        "--visual-tracking-max-pending",
        type=int,
        default=2,
    )
    parser.add_argument(
        "--visual-tracking-invert-x",
        action=argparse.BooleanOptionalAction,
        default=False,
    )
    parser.add_argument(
        "--visual-tracking-invert-y",
        action=argparse.BooleanOptionalAction,
        default=False,
    )
    parser.add_argument(
        "--find-owner-gain-x",
        type=float,
        default=1.0,
        help="Horizontal multiplier for explicit find_owner commands.",
    )
    parser.add_argument(
        "--find-owner-gain-y",
        type=float,
        default=0.8,
        help="Vertical multiplier for explicit find_owner commands.",
    )
    parser.add_argument(
        "--find-owner-stop-pixels",
        type=float,
        default=32.0,
        help="Stop moving when the detected face center is within this many pixels of frame center.",
    )
    parser.add_argument("--morrow-base-url", default=os.environ.get("MORROW_BASE_URL", "http://127.0.0.1:3000"))
    parser.add_argument("--morrow-session", default=os.environ.get("MORROW_SESSION", "default"))
    parser.add_argument("--morrow-auth-token", default=os.environ.get("MORROW_AUTH_TOKEN", ""))
    parser.add_argument("--morrow-connect-timeout", type=float, default=float(os.environ.get("MORROW_CONNECT_TIMEOUT_SECONDS", "10")))
    parser.add_argument("--morrow-turn-timeout", type=float, default=float(os.environ.get("MORROW_TURN_TIMEOUT_SECONDS", "120")))
    parser.add_argument("--morrow-reconnect-min", type=float, default=float(os.environ.get("MORROW_RECONNECT_MIN_SECONDS", "1")))
    parser.add_argument("--morrow-reconnect-max", type=float, default=float(os.environ.get("MORROW_RECONNECT_MAX_SECONDS", "30")))
    parser.add_argument(
        "--realtime-enabled",
        action=argparse.BooleanOptionalAction,
        default=parse_bool(os.environ.get("STACKCHAN_REALTIME_ENABLED", "true")),
        help="Enable the WebSocket realtime speech bridge.",
    )
    parser.add_argument("--realtime-ws-path", default=os.environ.get("STACKCHAN_REALTIME_WS_PATH", "/ws"))
    parser.add_argument(
        "--realtime-ws-port",
        type=int,
        default=int(os.environ.get("STACKCHAN_REALTIME_WS_PORT", "0")),
        help="Realtime WebSocket port. Defaults to HTTP port + 1 because the legacy HTTP server is stdlib-only.",
    )
    parser.add_argument("--realtime-public-host", default=os.environ.get("STACKCHAN_REALTIME_PUBLIC_HOST", ""))
    parser.add_argument("--realtime-local-token", default=os.environ.get("STACKCHAN_REALTIME_LOCAL_TOKEN", ""))
    parser.add_argument(
        "--ota-firmware-dir",
        default=os.environ.get("STACKCHAN_OTA_FIRMWARE_DIR", DEFAULT_OTA_FIRMWARE_DIR),
        help="Directory to scan for the newest valid ESP-IDF app .bin advertised through /ota.",
    )
    parser.add_argument(
        "--ota-firmware-file",
        default=os.environ.get("STACKCHAN_OTA_FIRMWARE_FILE", ""),
        help="Specific ESP-IDF app .bin to advertise through /ota. Overrides --ota-firmware-dir.",
    )
    parser.add_argument(
        "--ota-public-base-url",
        default=os.environ.get("STACKCHAN_OTA_PUBLIC_BASE_URL", ""),
        help="Public HTTP base URL used in firmware download links, for example http://192.168.1.20:8091.",
    )
    parser.add_argument(
        "--ota-force",
        action=argparse.BooleanOptionalAction,
        default=parse_bool(os.environ.get("STACKCHAN_OTA_FORCE", "false")),
        help="Ask devices to install the advertised firmware even when its version is not newer.",
    )
    parser.add_argument(
        "--ota-check-interval-seconds",
        type=int,
        default=int(os.environ.get("STACKCHAN_OTA_CHECK_INTERVAL_SECONDS", str(DEFAULT_OTA_CHECK_INTERVAL_SECONDS))),
        help="Interval for queuing check_ota commands to online devices. Set 0 to disable.",
    )
    parser.add_argument(
        "--sedentary-reminder-interval-seconds",
        type=int,
        default=int(
            os.environ.get(
                "STACKCHAN_SEDENTARY_REMINDER_INTERVAL_SECONDS",
                str(DEFAULT_SEDENTARY_REMINDER_INTERVAL_SECONDS),
            )
        ),
        help="Interval for face-gated sedentary reminders. Defaults to 1800 seconds; set 0 to disable.",
    )
    parser.add_argument("--aliyun-asr-ws-url", default=os.environ.get("STACKCHAN_ALIYUN_ASR_WS_URL", ""))
    parser.add_argument("--aliyun-tts-ws-url", default=os.environ.get("STACKCHAN_ALIYUN_TTS_WS_URL", ""))
    parser.add_argument("--audio-upstream-format", default=os.environ.get("STACKCHAN_AUDIO_UPSTREAM_FORMAT", "opus"))
    parser.add_argument("--aliyun-upstream-format", default=os.environ.get("STACKCHAN_ALIYUN_UPSTREAM_FORMAT", "pcm"))
    parser.add_argument(
        "--http-compat-enabled",
        action=argparse.BooleanOptionalAction,
        default=parse_bool(os.environ.get("STACKCHAN_HTTP_COMPAT_ENABLED", "true")),
        help="Keep legacy HTTP command and upload APIs available.",
    )
    args = parser.parse_args()
    if args.asr_sample_rate != 16000:
        log_print(
            f"设备上行ASR固定为16000Hz，忽略配置的采样率: {args.asr_sample_rate}",
            file=sys.stderr,
        )
        args.asr_sample_rate = 16000
    if args.sample_rate != 24000:
        log_print(
            f"设备下行音频固定为24000Hz，忽略配置的采样率: {args.sample_rate}",
            file=sys.stderr,
        )
        args.sample_rate = 24000

    httpd = AliyunVoiceServer((args.host, args.port), Handler)
    httpd.access_key_id = optional_env("ALIYUN_AK_ID", "ALIYUN_ACCESS_KEY_ID")
    httpd.access_key_secret = optional_env("ALIYUN_AK_SECRET", "ALIYUN_ACCESS_KEY_SECRET")
    httpd.token = optional_env("ALIYUN_NLS_TOKEN")
    httpd.token_expire_time = int(optional_env("ALIYUN_NLS_TOKEN_EXPIRE_TIME") or "0")
    if not httpd.token and (not httpd.access_key_id or not httpd.access_key_secret):
        raise SystemExit(
            "Missing Aliyun credentials. Set ALIYUN_NLS_TOKEN, or set ALIYUN_AK_ID and ALIYUN_AK_SECRET."
        )
    if not httpd.token:
        httpd.token, httpd.token_expire_time = create_aliyun_nls_token(
            httpd.access_key_id, httpd.access_key_secret
        )
    httpd.appkey = required_env("ALIYUN_NLS_APPKEY")
    httpd.asr_url = ASR_URLS[args.region]
    httpd.tts_url = args.tts_url or TTS_URLS[args.region]
    httpd.voice = args.voice
    httpd.asr_sample_rate = args.asr_sample_rate
    httpd.sample_rate = args.sample_rate
    httpd.volume = args.volume
    httpd.speaker_volume = clamp_speaker_volume(args.speaker_volume)
    httpd.speech_rate = args.speech_rate
    httpd.pitch_rate = args.pitch_rate
    httpd.max_sentence_chars = args.max_sentence_chars
    httpd.chunk_size = args.chunk_size
    httpd.tts_prefetch_workers = args.tts_prefetch_workers
    httpd.tts_request_timeout = args.tts_request_timeout
    httpd.tts_retries = args.tts_retries
    httpd.tts_tail_silence_ms = args.tts_tail_silence_ms
    httpd.command_queue_max_size = args.command_queue_max_size
    httpd.v3_database = Database(args.database_path)
    httpd.command_store = CommandStore(httpd.v3_database)
    recovered_boot_commands = httpd.command_store.expire_inactive_boot_commands()
    if recovered_boot_commands:
        log_print(f"启动时已过期旧 boot 命令: count={recovered_boot_commands}")
    recovered_dialogues = httpd.command_store.expire_stale_dialogue_commands()
    if recovered_dialogues:
        log_print(f"启动时已回收陈旧对话命令: count={recovered_dialogues}")
    httpd.device_registry = DeviceRegistry(httpd.v3_database)
    httpd.capture_save_mode = args.capture_save_mode
    httpd.save_audio_uploads = args.save_recording
    httpd.debug_log = args.debug
    httpd.capture_dir = args.capture_dir
    httpd.audio_capture_dir = os.path.join(args.capture_dir, "audio")
    httpd.device_log_dir = args.device_log_dir or os.path.join(args.capture_dir, "device-logs")
    httpd.static_dir = args.static_dir
    httpd.face_detector_backend = args.face_detector
    httpd.face_detector = None
    if args.face_detector == "yunet":
        httpd.face_detector = YunetFaceService(
            args.yunet_model,
            score_threshold=args.yunet_score_threshold,
            nms_threshold=args.yunet_nms_threshold,
            top_k=args.yunet_top_k,
        )
    httpd.visual_tracking_enabled = args.visual_tracking_enabled
    httpd.visual_tracking_deadzone_px = args.visual_tracking_deadzone_px
    httpd.visual_tracking_gain_x = args.visual_tracking_gain_x
    httpd.visual_tracking_gain_y = args.visual_tracking_gain_y
    httpd.visual_tracking_max_degree = args.visual_tracking_max_degree
    httpd.visual_tracking_min_degree = args.visual_tracking_min_degree
    httpd.visual_tracking_duration_ms = args.visual_tracking_duration_ms
    httpd.visual_tracking_min_interval_ms = args.visual_tracking_min_interval_ms
    httpd.visual_tracking_max_pending = args.visual_tracking_max_pending
    httpd.visual_tracking_invert_x = args.visual_tracking_invert_x
    httpd.visual_tracking_invert_y = args.visual_tracking_invert_y
    httpd.visual_tracking_last_command_at = {}
    httpd.find_owner_gain_x = args.find_owner_gain_x
    httpd.find_owner_gain_y = args.find_owner_gain_y
    httpd.find_owner_stop_pixels = args.find_owner_stop_pixels
    httpd.realtime_manager = None
    httpd.realtime_ws_path = args.realtime_ws_path
    httpd.realtime_ws_port = args.realtime_ws_port or (args.port + 1)
    httpd.realtime_public_host = args.realtime_public_host
    httpd.realtime_local_token = args.realtime_local_token
    httpd.ota_firmware_dir = args.ota_firmware_dir
    httpd.ota_firmware_file = args.ota_firmware_file
    httpd.ota_public_base_url = args.ota_public_base_url
    httpd.ota_force = args.ota_force
    httpd.ota_check_interval_seconds = args.ota_check_interval_seconds
    httpd.sedentary_reminder_interval_seconds = max(0, args.sedentary_reminder_interval_seconds)
    httpd.sedentary_reminder_trigger_total = 0
    httpd.sedentary_reminder_queued_total = 0
    httpd.device_lock = threading.Lock()
    httpd.device_queues = {}
    httpd.last_ack = {}
    httpd.last_seen = {}
    httpd.device_order = []
    httpd.dialog_awake_until = {}
    httpd.debug_config_lock = threading.Lock()
    httpd.wifi_log_enabled = args.wifi_logs
    httpd.usb_serial_enabled = args.usb_serial
    httpd.state_events_enabled = args.state_events
    httpd.device_config_poll_ms = max(1000, args.device_config_poll_ms)
    httpd.device_log_post_interval_ms = max(250, args.device_log_post_interval_ms)
    httpd.device_logs = {}
    httpd.recording_cache = []
    httpd.morrow_notice_stop_event = threading.Event()
    httpd.morrow_notice_thread = None

    def enqueue_morrow_command(device_id: str, command: dict) -> bool:
        return enqueue_server_command(httpd, device_id, command, persist=False)

    def cancel_morrow_generation(device_id: str, generation: int) -> int:
        httpd.command_store.set_speech_generation(device_id, generation)
        cancelled = httpd.command_store.cancel_pending_before_generation(device_id, generation)
        with httpd.device_lock:
            queue = httpd.device_queues.get(device_id)
        if queue is not None:
            cancelled += queue.discard_speech_before_generation(generation)
        return cancelled

    httpd.morrow_client = MorrowClient(
        base_url=args.morrow_base_url,
        session=args.morrow_session,
        auth_token=args.morrow_auth_token,
        connect_timeout=args.morrow_connect_timeout,
        reconnect_min=args.morrow_reconnect_min,
        reconnect_max=args.morrow_reconnect_max,
    ) if args.morrow_base_url else None
    httpd.morrow_web_gateway = MorrowWebGateway(
        base_url=args.morrow_base_url,
        default_session=args.morrow_session,
        auth_token=args.morrow_auth_token,
        connect_timeout=args.morrow_connect_timeout,
        turn_timeout=args.morrow_turn_timeout,
        device_session_switcher=(
            lambda session_id: httpd.morrow_client.switch_session(
                session_id,
                timeout=args.morrow_connect_timeout,
            )
        ) if httpd.morrow_client else None,
    )
    httpd.morrow_coordinator = MorrowTurnCoordinator(
        httpd.morrow_client,
        command_store_segment_sink(
            httpd.command_store,
            enqueue=enqueue_morrow_command,
            speaker_volume=lambda: httpd.speaker_volume,
        ),
        reply_end_sink=command_store_reply_end_sink(
            httpd.command_store,
            enqueue=enqueue_morrow_command,
        ),
        cancel_sink=cancel_morrow_generation,
        queue_size=8,
        request_ttl=60,
        turn_timeout=args.morrow_turn_timeout,
        max_segment_chars=args.max_sentence_chars,
        initial_generations=httpd.command_store.speech_generations(),
    ) if httpd.morrow_client else None
    if httpd.morrow_coordinator:
        if not httpd.morrow_client.check_status():
            log_print(f"Morrow 启动检查失败，将后台持续重连: {httpd.morrow_client.last_error}", file=sys.stderr)
        httpd.morrow_coordinator.start()

    def record_realtime_capture(metadata: dict) -> None:
        item = dict(metadata or {})
        item.setdefault("ts", time.time())
        with httpd.device_lock:
            httpd.recording_cache.append(item)
            if len(httpd.recording_cache) > DEVICE_RECORDING_MAX_ITEMS:
                del httpd.recording_cache[: len(httpd.recording_cache) - DEVICE_RECORDING_MAX_ITEMS]

    def enqueue_realtime_http_command(device_id: str, command: dict) -> bool:
        return enqueue_server_command(httpd, device_id, command)

    def record_realtime_device_connected(device_id: str) -> None:
        reset_device_logs_for_reconnect(httpd, device_id)

    prewarm_event_audio_cache(httpd, PREWARM_EVENT_AUDIO_NAMES)

    if args.realtime_enabled:
        realtime_config = RealtimeConfig(
            host=args.host,
            port=httpd.realtime_ws_port,
            path=args.realtime_ws_path,
            token=args.realtime_local_token,
            region=args.region,
            appkey=httpd.appkey,
            token_getter=httpd.get_token,
            aliyun_asr_ws_url=args.aliyun_asr_ws_url,
            aliyun_tts_ws_url=args.aliyun_tts_ws_url,
            voice=args.voice,
            upstream_sample_rate=args.asr_sample_rate,
            downstream_sample_rate=args.sample_rate,
            volume=args.volume,
            speech_rate=args.speech_rate,
            pitch_rate=args.pitch_rate,
            max_sentence_chars=args.max_sentence_chars,
            morrow_submit_callback=(
                (lambda device_id, text: httpd.morrow_coordinator.submit(text, device_id, source="voice"))
                if httpd.morrow_coordinator else None
            ),
            morrow_cancel_callback=(
                (lambda device_id: httpd.morrow_coordinator.cancel_device(device_id))
                if httpd.morrow_coordinator else None
            ),
            audio_capture_dir=httpd.audio_capture_dir,
            save_audio_uploads=httpd.save_audio_uploads,
            recording_callback=record_realtime_capture,
            command_callback=enqueue_realtime_http_command,
            speech_generation_callback=(
                (lambda device_id: httpd.morrow_coordinator.generation_for_device(device_id))
                if httpd.morrow_coordinator else None
            ),
            device_connected_callback=record_realtime_device_connected,
            debug=args.debug,
        )
        httpd.realtime_manager = RealtimeManager(realtime_config, logger=log_print)
        try:
            httpd.realtime_manager.start()
        except Exception as exc:
            httpd.realtime_manager = None
            log_print(f"实时语音服务启动失败: {exc}", file=sys.stderr)


    threading.Thread(target=run_periodic_ota_check_loop, args=(httpd,), name="ota-check", daemon=True).start()
    threading.Thread(
        target=run_periodic_sedentary_reminder_loop,
        args=(httpd,),
        name="sedentary-reminder",
        daemon=True,
    ).start()
    threading.Thread(target=run_morrow_notice_outbox_loop, args=(httpd,), name="morrow-outbox", daemon=True).start()
    if httpd.morrow_client:
        threading.Thread(target=run_morrow_notice_listener, args=(httpd,), name="morrow-notice-listener", daemon=True).start()

    log_print("小派服务已就绪")
    log_print(f"  人脸检测: {args.face_detector}")
    log_print(f"  截图保存模式: {args.capture_save_mode}")
    log_print(
        "  音频保存: "
        + (httpd.audio_capture_dir if httpd.save_audio_uploads else "禁用")
    )
    log_print(
        "  设备调试: "
        f"wifi_logs={'启用' if httpd.wifi_log_enabled else '禁用'} "
        f"usb_serial={'启用' if httpd.usb_serial_enabled else '禁用'} "
        f"state_events={'启用' if httpd.state_events_enabled else '禁用'}"
    )
    log_print(f"  设备日志文件: {httpd.device_log_dir}")
    log_print(f"  视觉跟踪: {'启用' if args.visual_tracking_enabled else '禁用'}")
    log_print(f"  命令队列: max_size={args.command_queue_max_size}")
    log_print(
        f"  音量: speaker={httpd.speaker_volume}% "
        f"aliyun_synthesis={args.volume}"
    )
    log_print(f"  Morrow: {'启用' if httpd.morrow_coordinator else '禁用'} session={args.morrow_session}")
    ota_firmware = find_latest_ota_firmware(httpd.ota_firmware_file, httpd.ota_firmware_dir)
    log_print(
        "  OTA 固件: "
        + (
            f"{ota_firmware.version} {ota_firmware.path}"
            if ota_firmware is not None
            else "禁用（未找到有效 app .bin）"
        )
    )
    log_print(
        "  OTA 检查命令: "
        + (f"每 {args.ota_check_interval_seconds}s" if args.ota_check_interval_seconds > 0 else "禁用")
    )
    log_print(
        "  定时久坐提醒: "
        + (
            f"每 {httpd.sedentary_reminder_interval_seconds}s（检测到人脸后播报）"
            if httpd.sedentary_reminder_interval_seconds > 0
            else "禁用"
        )
    )
    log_print(
        "  实时语音: "
        + (
            f"启用 ws://{args.host}:{httpd.realtime_ws_port}{args.realtime_ws_path}"
            if httpd.realtime_manager and httpd.realtime_manager.enabled
            else "禁用"
        )
    )
    log_print(f"  网页对话: http://{args.host}:{args.port}/web")
    if args.debug:
        log_print("  调试: 启用")
        log_print(f"  健康检查: http://127.0.0.1:{args.port}/health")
        log_print(f"  ASR:    http://{args.host}:{args.port}/upload")
        log_print(f"  TTS:    http://{args.host}:{args.port}/stream-speak?text=...")
        log_print(f"  事件音频: http://{args.host}:{args.port}/head-touch-events -> {args.static_dir}/event-audio")
        log_print(f"  图片上传: http://{args.host}:{args.port}/upload-image -> {args.capture_dir}")
        log_print(f"  OTA:    http://{args.host}:{args.port}/ota")
        log_print(f"  人脸检测详情: {args.face_detector}{' ' + args.yunet_model if args.face_detector == 'yunet' else ''}")
        log_print(
            f"  视觉跟踪详情: deadzone={args.visual_tracking_deadzone_px}px "
            f"gain_x={args.visual_tracking_gain_x} gain_y={args.visual_tracking_gain_y} "
            f"max_step={args.visual_tracking_max_degree}deg "
            f"invert_x={args.visual_tracking_invert_x} invert_y={args.visual_tracking_invert_y}"
        )
        log_print(
            f"  找人详情: gain_x={args.find_owner_gain_x} gain_y={args.find_owner_gain_y} "
            f"stop={args.find_owner_stop_pixels}px"
        )
        log_print(f"  Morrow 详情: {args.morrow_base_url or ''} session={args.morrow_session}")
        if httpd.realtime_manager and httpd.realtime_manager.enabled:
            log_print(f"  OTA:     http://{args.host}:{args.port}/ota")
            log_print(f"  实时 WS: ws://{args.host}:{httpd.realtime_ws_port}{args.realtime_ws_path}")
        log_print(f"  TTS 尾部静音: {args.tts_tail_silence_ms}ms")
        log_print("  通过 HTTP 长轮询推送命令:")
        log_print(f"          设备拉取: GET http://{args.host}:{args.port}/device/next-command?device_id=...")
        log_print(f"          发送命令: GET http://{args.host}:{args.port}/command/speak?device_id=...&text=...")
        log_print(
            f"  语音:  upstream ASR/Opus {args.asr_sample_rate}Hz; "
            f"downstream {args.voice} pcm_s16le/Opus {args.sample_rate}Hz mono"
        )
    try:
        httpd.serve_forever()
    finally:
        httpd.morrow_notice_stop_event.set()
        if httpd.realtime_manager:
            httpd.realtime_manager.stop()
        if getattr(httpd, "morrow_coordinator", None):
            httpd.morrow_coordinator.stop()
        if getattr(httpd, "morrow_client", None):
            httpd.morrow_client.stop()


if __name__ == "__main__":
    main()
