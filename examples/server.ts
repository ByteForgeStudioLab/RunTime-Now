// rtn HTTP server namunasi:  ./build/rtn examples/server.ts
interface Todo {
  id: number;
  title: string;
  done: boolean;
}

const todos: Todo[] = [
  { id: 1, title: "rtn runtime yozish", done: true },
  { id: 2, title: "HTTP server qo'shish", done: false },
];

const port = Number(process.env.PORT ?? 3000);

rtn.serve({ port, hostname: "127.0.0.1" }, async (req: Request): Promise<Response> => {
  const url = new URL(req.url);

  if (url.pathname === "/") {
    return new Response("Salom, RunTime-Now server!\n");
  }

  if (url.pathname === "/html") {
    return new Response("<h1>rtn ishlayapti 🚀</h1>", {
      headers: { "content-type": "text/html; charset=utf-8" },
    });
  }

  if (url.pathname === "/todos" && req.method === "GET") {
    return Response.json(todos);
  }

  if (url.pathname === "/todos" && req.method === "POST") {
    const body = (await req.json()) as Partial<Todo>;
    if (!body.title) return Response.json({ error: "title kerak" }, { status: 400 });
    const todo: Todo = { id: todos.length + 1, title: body.title, done: false };
    todos.push(todo);
    return Response.json(todo, { status: 201 });
  }

  if (url.pathname === "/salom") {
    const ism = url.searchParams.get("ism") ?? "mehmon";
    return new Response(`Salom, ${ism}!\n`);
  }

  if (url.pathname === "/sekin") {
    await new Promise((r) => setTimeout(r, 200));
    return new Response("200ms kutdim\n");
  }

  if (url.pathname === "/xato") {
    throw new Error("ataylab xato");
  }

  return new Response("Topilmadi\n", { status: 404 });
});
