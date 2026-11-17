"""Run the isolated development cluster until Ctrl+C; select business assemblies explicitly."""

import sys
from cluster_control import test_cluster


if __name__ == '__main__':
    sys.argv.append('--foreground')
    try:
        raise SystemExit(test_cluster.main())
    except (OSError, RuntimeError) as error:
        print(f'Cluster failed: {error}', file=sys.stderr)
        raise SystemExit(1)
