FROM oven/bun:1

WORKDIR /app

ARG GIT_COMMIT_HASH=unknown
ENV GIT_COMMIT_HASH=${GIT_COMMIT_HASH}

COPY package.json ./
COPY ui.ts api.ts ./
COPY docker/start.sh /usr/local/bin/start-remote-timer

RUN chmod +x /usr/local/bin/start-remote-timer

EXPOSE 3000 3001

CMD ["start-remote-timer"]
