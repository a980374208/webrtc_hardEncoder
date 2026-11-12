import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('prefix', type=Path)
args = parser.parse_args()
path = Path(str(args.prefix) + '.resources.csv')
control = json.loads(Path(str(args.prefix) + '.json').read_text())
rows = list(csv.DictReader(path.open()))
measured = [r for r in rows if r['phase'] == 'measurement-release']
fields = {'private_bytes': 32 * 1024 * 1024, 'handles': 8,
          'gpu_local_usage_bytes': 8 * 1024 * 1024}
errors, trends = [], {}
if control['status'] != 'PASS' or control['completed_cycles'] != control['requested_cycles']:
    errors.append('Serial control/lifecycle gate did not pass')
if len(measured) < 6:
    errors.append('Fewer than six independent after-release resource samples')
if any(r['process_valid'] != '1' or r['gpu_valid'] != '1' for r in rows):
    errors.append('Process/GPU resource sampling incomplete')
for field, limit in fields.items():
    head = statistics.median(int(r[field]) for r in measured[:3]) if measured else 0
    tail = statistics.median(int(r[field]) for r in measured[-3:]) if measured else 0
    growth = tail - head
    trends[field] = {'head_three_cycle_median': head, 'tail_three_cycle_median': tail,
                     'growth': growth, 'maximum_allowed_growth': limit}
    if growth > limit or (field == 'handles' and abs(growth) > limit):
        errors.append(field + ' exceeded predeclared resource trend limit')
report = {'status': 'PASS' if not errors else 'FAIL', 'errors': errors, 'trends': trends,
          'scope': 'Finite serial lifecycle resource trend after double Release; no claim of physical device loss or long stability',
          'cycles': control['requested_cycles'], 'after_release_samples': len(measured),
          'resource_csv_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
          'analyzer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
destination = Path(str(args.prefix) + '.resource-gate.json')
destination.write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if not errors else 1)
