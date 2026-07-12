curl -X POST 'http://127.0.0.1:8091/command' \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "speak",
    "payload": {
      "text": "哈喽，我是小派。",
      "voice": "zhiyan_emo"
    },
    "interrupt": true
  }'

#   zhimi_emo