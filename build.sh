set -e
set -o pipefail

cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic hw5-asm.c -o hw5-asm
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic hw5-sim.c -o hw5-sim
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic test_hw5.c -o test_hw5