set -euo pipefail

cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic hw5-asm.c -o hw5-asm -lm
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic hw5-sim.c -o hw5-sim -lm