# etermin-watch

Small C program that polls an eTermin booking endpoint and prints (or notifies about)
free appointment slots inside a configurable time window.

## Build
    sudo apt install build-essential libcurl4-openssl-dev libcjson-dev   # Debian/Ubuntu
    make

## Configure
    cp config.ini.example config.ini
    ./etermin_watch config.ini

* `min_hours_ahead` / `max_days_ahead` define the relevant window from "now".
* One `[target:NAME]` section per doctor/service.
* `interval_minutes = 0` runs once; otherwise it keeps polling and reports only NEW slots.
* `notify_cmd` runs for each new slot with the text in `$ETERMIN_MSG`
  (e.g. `notify-send`, `curl` to ntfy.sh, a mail command).

## Finding the endpoint
eTermin's booking pages load slots via an HTTP call. Open the booking page, press F12 ->
Network tab, pick the service/doctor, and copy the request that returns the free times
(URL + any headers) into `url` / `header`. Replace the date with `{date}`, and use
`{from}`, `{to}`, `{days}` if the request has a range. Then set `start_field`
(and `array_key` if the list is nested) to match the JSON you see.

## Notes
* Personal use only: keep the interval reasonable (5+ min) and check the site's terms.
* The official eTermin API needs keys from the account owner, so it is normally not
  usable for booking as a patient; the booking-page request above is the practical route.
* Time strings are interpreted as local time.
