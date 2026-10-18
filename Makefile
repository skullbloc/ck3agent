.PHONY: help bridge test wheel doctor export-check
.DEFAULT_GOAL := help

help:  ## list targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | sed 's/:.*## /\t/'

bridge:  ## build the default target as CI does (library, i386 stub, native tests), then ctest
	cmake -S bridge -B bridge/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCK3_STRICT_BUILD_ID=ON -DBUILD_BRIDGE_TESTS=ON && cmake --build bridge/build -j$$(nproc) && cd bridge/build && ctest --output-on-failure

test:  ## game-free Python tests (server suite + CLI)
	python3 -m pytest

wheel:  ## build wheel and sdist into dist/
	python3 -m build

doctor:  ## check the whole chain (game, bridge, socket)
	ck3agent doctor

export-check:  ## VERSION, package version and BuildID agree
	python3 -c "import ck3agent as c; from ck3agent._bridge_meta import SUPPORTED_BUILD_ID as b; assert open('VERSION').read().strip() == c.__version__, 'VERSION != package'; assert b in open('re/offsets/offset_version.h').read(), 'BuildID mismatch'; print('ok', c.__version__, b[:8])"
