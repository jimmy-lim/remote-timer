FROM oven/bun:1

WORKDIR /app

COPY package.json ./
COPY ui.ts api.ts ./

EXPOSE 3000 3001
