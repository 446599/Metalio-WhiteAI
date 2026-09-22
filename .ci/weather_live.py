"""Public provider connectivity probe; no device credentials or user location."""
import json
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import urlopen
out=Path('validation');out.mkdir(exist_ok=True)
def fetch(url):
    with urlopen(url, timeout=15) as response:
        data=response.read(16385)
        assert response.status==200 and len(data)<=16384
        return json.loads(data)
geo=fetch('https://geocoding-api.open-meteo.com/v1/search?'+urlencode({'name':'Shanghai','count':5,'language':'zh','format':'json'}))
place=geo['results'][0]
query={'latitude':f"{place['latitude']:.5f}",'longitude':f"{place['longitude']:.5f}",'current':'temperature_2m,relative_humidity_2m,apparent_temperature,wind_speed_10m,weather_code','temperature_unit':'celsius','wind_speed_unit':'kmh','timeformat':'unixtime','forecast_days':1}
forecast=fetch('https://api.open-meteo.com/v1/forecast?'+urlencode(query))
units=forecast['current_units']
assert units['time']=='unixtime' and units['temperature_2m']=='°C' and units['wind_speed_10m']=='km/h'
assert isinstance(forecast['current']['time'],int)
(out/'weather-live.json').write_text(json.dumps({'probe_city':'Shanghai (public test, not user location)','geocoding':geo,'forecast':forecast},ensure_ascii=False,indent=2)+'\n')
print('Public Geocoding/Forecast HTTPS responses and units verified; not a device radio test.')
