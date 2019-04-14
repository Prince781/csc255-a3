void foo() {
    int a[1000][1000][1000];
    int sum = 0;

    for (int i = 7; i*i < 1000; ++i) {
        for (int j = 3; j*j < 500; ++j) {
            for (int k = 9; k*k < 1000; ++k) {
                a[i][j][j + k] = 4;
                sum += a[i][j-1][k+2];
            }
        }
    }
}

int bar() {
	int sum = 0;
	for (int j = 0; j < 100; j++) {
            for (int k = 0; k < 100; k++) {
		sum += j;
                sum += k;
            }
	}
	return sum;
}

int main() {
    foo();
    bar();
}
