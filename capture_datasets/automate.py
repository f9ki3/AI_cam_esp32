import os
import time
import hashlib
import argparse
import urllib.request

BASE_URL_DEFAULT = "http://10.56.79.155/capture"  # expects: /capture?jpeg=1 or similar


def sha1_hex(s: str) -> str:
    return hashlib.sha1(s.encode("utf-8")).hexdigest()


def fetch_jpeg(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "AI_cam_esp32/automate"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.read()


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def is_likely_jpeg(data: bytes) -> bool:
    return len(data) >= 4 and data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"


def main():
    parser = argparse.ArgumentParser(description="Capture N JPEG frames from ESP32-CAM and save into hazardous/non_hazardous folders.")
    parser.add_argument("--base-url", default=BASE_URL_DEFAULT, help="Capture endpoint base URL (default: http://IP/capture)")
    parser.add_argument("--count", type=int, default=100, help="Number of images to capture")
    parser.add_argument("--label", choices=["hazardous", "non_hazardous"], default="no_object", help="Destination label folder")
    parser.add_argument("--out-root", default=os.path.join(os.path.dirname(__file__), ""), help="Root folder to store datasets")
    parser.add_argument("--delay", type=float, default=0.2, help="Delay between captures (seconds)")
    parser.add_argument("--start-index", type=int, default=0, help="Starting index for filenames")
    args = parser.parse_args()

    out_root = os.path.abspath(args.out_root)
    dest_dir = os.path.join(out_root, args.label)
    ensure_dir(dest_dir)

    # Construct a URL that is likely to work with common ESP32-CAM capture endpoints.
    # If your firmware uses a different query param, adjust --base-url accordingly.
    # We try two common patterns:
    #   1) /capture?format=jpeg
    #   2) /capture (raw JPEG)
    candidate_urls = [
        f"{args.base_url}?format=jpeg",
        f"{args.base_url}?jpeg=1",
        f"{args.base_url}",
    ]

    print(f"Destination: {dest_dir}")
    print(f"Capturing {args.count} images...")

    for i in range(args.count):
        idx = args.start_index + i
        ts = time.strftime("%Y%m%d-%H%M%S")
        filename = f"img_{idx:04d}_{ts}.jpg"
        out_path = os.path.join(dest_dir, filename)

        last_err = None
        data = None
        for u in candidate_urls:
            try:
                print(f"[{i+1}/{args.count}] Fetching: {u}")
                data = fetch_jpeg(u)
                if is_likely_jpeg(data):
                    break
                # If not JPEG, keep trying other URLs.
                last_err = ValueError("Response did not look like a JPEG")
            except Exception as e:
                last_err = e
                data = None

        if data is None:
            raise RuntimeError(f"Failed to capture image #{idx}: {last_err}")

        with open(out_path, "wb") as f:
            f.write(data)

        # Small delay to avoid hammering the camera.
        if args.delay > 0:
            time.sleep(args.delay)

    print("Done.")


if __name__ == "__main__":
    main()

