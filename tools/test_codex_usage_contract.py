import json


def parse(payload: str) -> dict:
    data = json.loads(payload)
    today = data["usage"]["today"]
    models = data["model_stats"]
    history = sorted(data["daily_usage"], key=lambda item: item["date"], reverse=True)[:7]
    cached = today.get("cache_read_tokens", 0) + today.get("cache_creation_tokens", 0)
    sub = data.get("subscription", {})
    top = max(models, key=lambda item: item["cost"])["model"] if models else "NO USAGE"
    limit = sub.get("daily_limit_usd", 0)
    return {
        "cost": today["cost"],
        "limit": limit,
        "weekly_usage": sub.get("weekly_usage_usd") or 0,
        "weekly_limit": sub.get("weekly_limit_usd") or 0,
        "input": today["input_tokens"],
        "cached": cached,
        "output": today["output_tokens"],
        "requests": today["requests"],
        "top_model": top,
        "history_dates": [item["date"] for item in history],
    }


def test_contract() -> None:
    result = parse(
        '{"isValid":true,"subscription":{"daily_limit_usd":300,'
        '"weekly_usage_usd":62.99,"weekly_limit_usd":700},'
        '"usage":{"today":{"cost":5.89,"input_tokens":333681,"output_tokens":9641,'
        '"cache_read_tokens":2072576,"cache_creation_tokens":0,"requests":30}},'
        '"model_stats":[{"model":"gpt-6-astra","cost":5.89,"requests":30,'
        '"input_tokens":333681,"output_tokens":9641,"cache_read_tokens":2072576,'
        '"cache_creation_tokens":0,"total_tokens":2415898}],'
        '"daily_usage":[{"date":"2026-09-11","cost":5.89,"input_tokens":333681,'
        '"output_tokens":9641,"cache_read_tokens":2072576,"requests":30,'
        '"total_tokens":2415898}]}'
    )
    assert result == {
        "cost": 5.89,
        "limit": 300,
        "weekly_usage": 62.99,
        "weekly_limit": 700,
        "input": 333681,
        "cached": 2072576,
        "output": 9641,
        "requests": 30,
        "top_model": "gpt-6-astra",
        "history_dates": ["2026-09-11"],
    }
    empty = parse(
        '{"isValid":true,"usage":{"today":{"cost":0,"input_tokens":0,"output_tokens":0,'
        '"requests":0}},"model_stats":[],"daily_usage":[]}'
    )
    assert empty["top_model"] == "NO USAGE" and empty["cost"] == 0 and empty["limit"] == 0
    assert empty["weekly_usage"] == 0 and empty["weekly_limit"] == 0


if __name__ == "__main__":
    test_contract()
