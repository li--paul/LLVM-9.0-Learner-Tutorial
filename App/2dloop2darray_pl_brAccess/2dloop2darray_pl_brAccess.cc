void f ( int A[50][100]) {
  int N = 100;
  int M = 50;
  int B[50][100];
  for ( int j = 0; j < N; j++ )
    for ( int i = 0; i < M; i++ )
      B[i][j]=i;
  for ( int j = 1; j < N; j++ )
    for ( int i = 1; i < M; i++ )
      A[i][j] = i%2? A[i-1][j]:B[i][j];
  return;
}