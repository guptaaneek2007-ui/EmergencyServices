FROM gcc:latest

WORKDIR /app

COPY . .

RUN apt-get update && apt-get install -y default-libmysqlclient-dev

RUN gcc p6.c mongoose.c -o p6 -lmysqlclient

CMD ["./p6"]